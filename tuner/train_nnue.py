#!/usr/bin/env python3
"""
train_nnue.py — PyTorch NNUE Trainer, Quantization Binary Exporter & Parity Verifier for GC-Chess-Engine.

Supports both:
  1. GCNNUE05 (Default — GC-RelGeo Dual-Stream CP-HG + Dual-Head V * (1 - DrawGate)):
     - Stream A (Incremental): 768 -> 512 x 2 with 3-Zone CP-HG activation (1024)
     - Stream B (Sparse Leaf RelGeo + HMC): 2619 -> 128 with SCReLU (Canonical A-D king mirror)
     - Fusion Bottleneck: 1152 -> 32 (int8 weights) with SCReLU
     - Dual-Head Smooth Bucketed Output (8 buckets):
         V(b) = SkipLinear_A[b](1024) + MLP_V[b](32)
         DrawGate(b) = sigmoid(MLP_D[b](32))
         Eval = V(b) * (1 - DrawGate(b))
     - Exact Binary Size: 1,512,664 bytes ("GCNNUE05")
  2. GCNNUE04 (Legacy — Unified CP-HG 768 -> 512 x 2 -> 8 buckets, 803,880 bytes):
     - Compatible for warm-starting GCNNUE05 (--init-from gc.nnue) or training v4 (--arch v4).
"""

import argparse
import ctypes
import glob
import gzip
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim

# ==============================================================================
# Constants & Specifications
# ==============================================================================

MAGIC_HEADER_V2 = b"GCNNUE02"
MAGIC_HEADER_V3 = b"GCNNUE03"
MAGIC_HEADER_V4 = b"GCNNUE04"
MAGIC_HEADER_V5 = b"GCNNUE05"
MAGIC_HEADER = MAGIC_HEADER_V5
QA = 255
QB = 64
SCALE = 400
EXPECTED_FILE_SIZE_V2 = 789516   # 8 + 786432 + 1024 + 2048 + 4 bytes
EXPECTED_FILE_SIZE_V3 = 803880   # 8 + 786432 + 1024 + 16384 + 32 bytes
EXPECTED_FILE_SIZE_V4 = 803880   # 8 + 786432 + 1024 + 16384 + 32 bytes
EXPECTED_FILE_SIZE_V5 = 1512664  # 8 + 16 + StreamA + StreamB + MLP1 + DualHead
EXPECTED_FILE_SIZE = EXPECTED_FILE_SIZE_V5
NUM_FEATURES = 768
HIDDEN_SIZE = 512
OUTPUT_INPUT_SIZE = HIDDEN_SIZE * 2  # 1024
OUTPUT_BUCKETS = 8

RELGEO_FEATURES = 2619
RELGEO_HIDDEN = 128
MLP_INPUT_SIZE = OUTPUT_INPUT_SIZE + RELGEO_HIDDEN  # 1152
MLP_HIDDEN = 32
HMC_THRESH = (8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96)


def get_output_bucket(num_pieces):
    """
    Computes base output bucket index [0..7] based on total piece count:
      bucket = clamp((num_pieces - 1) // 4, 0, 7)
    """
    b = (num_pieces - 1) // 4
    return max(0, min(7, b))

PIECE_MAP = {
    'p': 1,  # PAWN
    'n': 2,  # KNIGHT
    'b': 3,  # BISHOP
    'r': 4,  # ROOK
    'q': 5,  # QUEEN
    'k': 6   # KING
}


# ==============================================================================
# Feature Extraction & FEN Parsing
# ==============================================================================

def parse_fen_to_features(fen_str):
    """
    Parses a FEN board representation into active feature indices for White and Black POV.

    Feature Indexing Formula (Spatially Aligned 768 for Cross-Perspective Gating):
      For perspective persp in {0 (White), 1 (Black)}, piece color c in {0, 1},
      piece type pt in {1..6}, square sq in [0, 63]:
        color_rel = c ^ persp
        sq_rel = sq if persp == 0 else (sq ^ 56)
        FeatureIdx = color_rel * 384 + (pt - 1) * 64 + sq_rel

    Returns:
      w_indices (list[int]): Active features for White perspective
      b_indices (list[int]): Active features for Black perspective
      stm (int): Side to move (0 = White, 1 = Black)
    """
    tokens = fen_str.split()
    board_part = tokens[0]
    stm = 0 if len(tokens) > 1 and tokens[1] == 'w' else 1

    w_indices = []
    b_indices = []

    rank = 7
    file = 0
    for ch in board_part:
        if ch == '/':
            rank -= 1
            file = 0
        elif ch.isdigit():
            file += int(ch)
        else:
            c = 0 if ch.isupper() else 1
            pt = PIECE_MAP[ch.lower()]
            sq = rank * 8 + file

            w_idx = c * 384 + (pt - 1) * 64 + sq
            w_indices.append(w_idx)

            b_idx = (c ^ 1) * 384 + (pt - 1) * 64 + (sq ^ 56)
            b_indices.append(b_idx)

            file += 1

    return w_indices, b_indices, stm


def parse_relgeo_features(fen_str):
    """
    Parses a FEN into Stream A (768 w/b indices, stm) + Stream B RelGeo indices,
    piece_count, and halfmove_clock, matching C++ NNUE::extract_relgeo_features bit-for-bit.
    """
    tokens = fen_str.split()
    board_part = tokens[0]
    stm = 0 if len(tokens) > 1 and tokens[1] == 'w' else 1
    hmc = int(tokens[4]) if len(tokens) > 4 and tokens[4].isdigit() else 0

    w_indices = []
    b_indices = []
    pieces = []
    kings = [0, 0]

    rank = 7
    file = 0
    for ch in board_part:
        if ch == '/':
            rank -= 1
            file = 0
        elif ch.isdigit():
            file += int(ch)
        else:
            c = 0 if ch.isupper() else 1
            pt = PIECE_MAP[ch.lower()]
            sq = rank * 8 + file
            w_indices.append(c * 384 + (pt - 1) * 64 + sq)
            b_indices.append((c ^ 1) * 384 + (pt - 1) * 64 + (sq ^ 56))
            pieces.append((c, pt, sq))
            if pt == 6:
                kings[c] = sq
            file += 1

    flip_v = 56 if stm == 1 else 0
    wk_persp = kings[stm] ^ flip_v
    mirror_h = 7 if (wk_persp & 7) < 4 else 0
    flip_all = flip_v ^ mirror_h

    def is_light(sq):
        return (((sq & 7) + (sq >> 3)) & 1) != 0

    ok_sq = kings[stm] ^ flip_all
    ek_sq = kings[stm ^ 1] ^ flip_all
    ok_f, ok_r = ok_sq & 7, ok_sq >> 3
    ek_f, ek_r = ek_sq & 7, ek_sq >> 3

    rel_indices = [
        (ek_r - ok_r + 7) * 15 + (ek_f - ok_f + 7),
        225 + ok_sq,
        289 + ek_sq,
    ]

    stm_b_lights = []
    nstm_b_lights = []
    has_other = False
    for c, pt, sq in pieces:
        if pt == 6:
            continue
        psq = sq ^ flip_all
        pf, pr = psq & 7, psq >> 3
        c_rel = 0 if c == stm else 1
        rel_indices.append(353 + (c_rel * 5 + (pt - 1)) * 225 + (pr - ek_r + 7) * 15 + (pf - ek_f + 7))
        if pt == 3:
            if c == stm:
                stm_b_lights.append(is_light(psq))
            else:
                nstm_b_lights.append(is_light(psq))
        elif pt in (2, 4, 5):
            has_other = True

    if any(stm_b_lights):
        rel_indices.append(2603)
    if any(not x for x in stm_b_lights):
        rel_indices.append(2604)
    if is_light(ek_sq):
        rel_indices.append(2605)
    if (not has_other) and len(stm_b_lights) == 1 and len(nstm_b_lights) == 1 and stm_b_lights[0] != nstm_b_lights[0]:
        rel_indices.append(2606)

    for k, thresh in enumerate(HMC_THRESH):
        if hmc >= thresh:
            rel_indices.append(2607 + k)

    return w_indices, b_indices, stm, rel_indices, len(pieces), min(255, hmc)


def parse_data_line(line, lambda_wdl=0.65):
    """
    Parses a line from the dataset formatted as 'FEN | score | wdl' (or pure FEN).

    Score in dataset is White POV (centipawns).
    WDL in dataset is White POV (1.0 = Win, 0.5 = Draw, 0.0 = Loss).

    Returns:
      (w_indices, b_indices, stm, target, norm_fen, piece_count, hmc, wdl_stm, score_stm) or None if malformed.
    """
    line = line.strip()
    if not line or line.startswith('#'):
        return None

    parts = line.split('|')
    fen = parts[0].strip()
    if len(parts) >= 3:
        try:
            score = float(parts[1].strip())
            wdl = float(parts[2].strip())
        except ValueError:
            return None
    else:
        score = 0.0
        wdl = 0.5

    tokens = fen.split()
    if len(tokens) < 2:
        return None

    stm = 0 if tokens[1] == 'w' else 1
    hmc = int(tokens[4]) if len(tokens) > 4 and tokens[4].isdigit() else 0

    # Convert score and wdl to side-to-move perspective
    score_stm = score if stm == 0 else -score
    wdl_stm = wdl if stm == 0 else (1.0 - wdl)

    # Sigmoid winning probability from score: P = sigmoid(score_stm / 400.0)
    scaled_score = max(-15.0, min(15.0, score_stm / 400.0))
    p_score = 1.0 / (1.0 + math.exp(-scaled_score))

    # Blended loss target: (1 - lambda) * P_score + lambda * WDL_stm
    target = (1.0 - lambda_wdl) * p_score + lambda_wdl * wdl_stm

    # Normalized FEN for deduplication (board, stm, castling, ep, hmc when present)
    norm_fen = " ".join(tokens[:5]) if len(tokens) >= 5 else " ".join(tokens[:4])

    w_indices, b_indices, _ = parse_fen_to_features(fen)
    piece_count = max(2, min(32, len(w_indices)))
    return w_indices, b_indices, stm, target, norm_fen, piece_count, min(255, hmc), wdl_stm, score_stm


# ==============================================================================
# Fast In-Memory Dataset
# ==============================================================================

class ChessNNUEDataset:
    """
    Compact, cache-friendly in-memory dataset storing active feature indices.
    Each chess position has at most 32 pieces, enabling fast vectorized batch collation
    and on-the-fly GPU RelGeo extraction without duplicating dataset RAM.
    """
    def __init__(self, w_indices, b_indices, stm, targets, buckets, hmc=None, wdl=None, cp=None):
        self.w_indices = w_indices  # torch.int16 tensor of shape (N, 32)
        self.b_indices = b_indices  # torch.int16 tensor of shape (N, 32)
        self.stm = stm              # torch.uint8 tensor of shape (N,)
        self.targets = targets      # torch.float32 tensor of shape (N,)
        self.buckets = buckets      # torch.uint8 tensor of shape (N,)
        n = len(stm)
        self.hmc = hmc if hmc is not None else torch.zeros(n, dtype=torch.uint8)
        self.wdl = wdl if wdl is not None else targets
        self.cp = cp if cp is not None else torch.zeros(n, dtype=torch.float32)
        self.length = n

    def __len__(self):
        return self.length


def open_file_stream(filepath):
    """Opens a plain text or gzip-compressed file transparently."""
    if str(filepath).endswith(".gz"):
        return gzip.open(filepath, "rt", encoding="utf-8", errors="ignore")
    return open(filepath, "r", encoding="utf-8", errors="ignore")


def resolve_dataset_files(data_sources=None):
    """
    Discovers all dataset files from single/multiple paths, directories (searched recursively),
    or glob patterns.

    Supports:
      - None or empty: Auto-detects in Kaggle (/kaggle/input, /kaggle/working) or local paths (output/data, data).
      - Directory (e.g. /kaggle/input): Recursively discovers all .txt, .epd, .txt.gz files across all subfolders.
      - Multiple sources: List/tuple of paths or comma-separated strings.
      - Wildcards: e.g. /kaggle/input/**/*.txt.
    """
    raw_sources = []
    if data_sources is None:
        raw_sources = []
    elif isinstance(data_sources, (list, tuple)):
        for item in data_sources:
            if isinstance(item, str) and (',' in item or ';' in item):
                raw_sources.extend([s.strip() for s in item.replace(';', ',').split(',') if s.strip()])
            elif item:
                raw_sources.append(item)
    elif isinstance(data_sources, str):
        if ',' in data_sources or ';' in data_sources:
            raw_sources.extend([s.strip() for s in data_sources.replace(';', ',').split(',') if s.strip()])
        elif data_sources.strip():
            raw_sources.append(data_sources.strip())
    else:
        raw_sources.append(str(data_sources))

    # Auto-detection if no sources specified or empty list passed
    if not raw_sources:
        candidates = [
            "/kaggle/input",
            "/kaggle/working/data",
            "/kaggle/input/datasets/ramadhanigb19/nnue-dataset",
            "/kaggle/input/datasets/ramadhanigb19/nnue-dataset/dataset_all.txt",
            "/kaggle/working",
            "output/data",
            "data",
            "."
        ]
        found_auto = []
        for cand_str in candidates:
            cand = Path(cand_str)
            if cand.is_file() and cand.suffix.lower() in ('.txt', '.epd'):
                found_auto.append(cand.resolve())
                break
            elif cand.is_dir():
                sub_files = []
                for ext in ('*.txt', '*.epd', '*.txt.gz'):
                    sub_files.extend(cand.rglob(ext))
                filtered = [
                    f.resolve() for f in sub_files
                    if f.is_file() and f.stat().st_size > 0
                    and f.name.lower() not in ('requirements.txt', 'baseline.txt', 'cmakelists.txt', 'wac_miss.txt')
                ]
                if filtered:
                    found_auto.extend(filtered)
                    break
        if not found_auto:
            raise FileNotFoundError(
                "No dataset file specified and none found in auto-detect locations "
                "(/kaggle/input, /kaggle/working, output/data, data). "
                "Please specify using --data <path_or_dir>."
            )
        raw_sources = found_auto

    files_to_read = []
    seen_paths = set()

    def add_file(p):
        rp = Path(p).resolve()
        if rp.is_file() and rp.stat().st_size > 0 and str(rp) not in seen_paths:
            if rp.name.lower() in ('requirements.txt', 'baseline.txt', 'cmakelists.txt', 'wac_miss.txt'):
                return
            seen_paths.add(str(rp))
            files_to_read.append(rp)

    for src in raw_sources:
        src_str = str(src)
        if any(c in src_str for c in ('*', '?', '[')):
            matches = glob.glob(src_str, recursive=True)
            for m in sorted(matches):
                add_file(m)
            continue

        p = Path(src_str)
        if p.is_file():
            add_file(p)
        elif p.is_dir():
            dir_files = []
            for ext in ('*.txt', '*.epd', '*.txt.gz'):
                dir_files.extend(p.rglob(ext))

            by_parent = {}
            for f in dir_files:
                parent = f.parent.resolve()
                by_parent.setdefault(parent, []).append(f)

            for parent, flist in by_parent.items():
                has_all = any(f.name == 'dataset_all.txt' for f in flist)
                for f in flist:
                    # In a directory where dataset_all.txt is present, skip chunk_*.txt to prevent duplicate parsing
                    if has_all and f.name.startswith('chunk_') and f.name.endswith('.txt'):
                        continue
                    add_file(f)
        else:
            if p.resolve().is_file():
                add_file(p)

    # Deduplicate chunk_*.txt when dataset_all.txt is present across the discovered files
    by_parent = {}
    for f in files_to_read:
        by_parent.setdefault(f.parent.resolve(), []).append(f)

    deduped_files = []
    for parent, flist in by_parent.items():
        has_all = any(f.name == 'dataset_all.txt' for f in flist)
        for f in flist:
            if has_all and f.name.startswith('chunk_') and f.name.endswith('.txt'):
                print(f"[*] Skipping {f.name} (already consolidated in dataset_all.txt)")
                continue
            deduped_files.append(f)

    return sorted(deduped_files, key=lambda x: str(x))


# ==============================================================================
# High-Speed Compiled C++ Dataset Parser Extension
# ==============================================================================

CPP_FAST_LOADER_SOURCE = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

extern "C" {

#define BLOCK_CAP 65536

struct Block {
    int16_t w[BLOCK_CAP][32];
    int16_t b[BLOCK_CAP][32];
    uint8_t stm[BLOCK_CAP];
    uint8_t bucket[BLOCK_CAP];
    uint8_t hmc[BLOCK_CAP];
    float tgt[BLOCK_CAP];
    float wdl[BLOCK_CAP];
    float cp[BLOCK_CAP];
    int count;
};

static std::vector<Block*> g_blocks;
static int g_total_count = 0;
static int g_total_lines = 0;

static uint64_t* g_hash_table = nullptr;
static uint32_t g_hash_mask = 0;
static uint32_t g_hash_count = 0;

static inline uint64_t hash64(const char* str, int len) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < len; ++i) {
        h ^= (uint8_t)str[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void rehash(uint32_t new_cap) {
    uint64_t* old_table = g_hash_table;
    uint32_t old_cap = g_hash_mask ? (g_hash_mask + 1) : 0;

    g_hash_table = (uint64_t*)calloc(new_cap, sizeof(uint64_t));
    g_hash_mask = new_cap - 1;
    g_hash_count = 0;

    if (old_table) {
        for (uint32_t i = 0; i < old_cap; ++i) {
            uint64_t h = old_table[i];
            if (h != 0) {
                uint32_t idx = (uint32_t)(h & g_hash_mask);
                while (g_hash_table[idx] != 0) {
                    idx = (idx + 1) & g_hash_mask;
                }
                g_hash_table[idx] = h;
                g_hash_count++;
            }
        }
        free(old_table);
    }
}

static inline bool insert_hash(uint64_t h) {
    if (h == 0) h = 1;
    if (g_hash_count * 2 >= g_hash_mask) {
        uint32_t new_cap = g_hash_mask ? ((g_hash_mask + 1) * 2) : (1 << 20);
        rehash(new_cap);
    }
    uint32_t idx = (uint32_t)(h & g_hash_mask);
    while (g_hash_table[idx] != 0) {
        if (g_hash_table[idx] == h) return false; // Duplicate
        idx = (idx + 1) & g_hash_mask;
    }
    g_hash_table[idx] = h;
    g_hash_count++;
    return true; // Inserted
}

void fast_loader_reset(int reserve_entries) {
    for (Block* b : g_blocks) {
        delete b;
    }
    g_blocks.clear();
    
    if (g_hash_table) {
        free(g_hash_table);
        g_hash_table = nullptr;
    }
    g_hash_mask = 0;
    g_hash_count = 0;

    uint32_t init_cap = 1 << 20;
    uint32_t target = (reserve_entries > 0) ? ((uint32_t)reserve_entries * 2) : init_cap;
    if (target > (1U << 27)) target = (1U << 27); // Cap initial allocation at 128M slots (~1GB)
    while (init_cap < target && init_cap < (1U << 28)) {
        init_cap <<= 1;
    }
    rehash(init_cap);

    g_total_count = 0;
    g_total_lines = 0;
}

int fast_loader_read_file(const char* filepath, float lambda_wdl, int max_samples, int* out_file_lines, int* out_file_kept) {
    FILE* f = fopen(filepath, "r");
    if (!f) return -1;

    char buffer[131072];
    setvbuf(f, buffer, _IOFBF, sizeof(buffer));

    int PIECE_TABLE_W[256];
    int PIECE_TABLE_B[256];
    memset(PIECE_TABLE_W, 0, sizeof(PIECE_TABLE_W));
    memset(PIECE_TABLE_B, 0, sizeof(PIECE_TABLE_B));

    PIECE_TABLE_W['P'] = 0;   PIECE_TABLE_B['P'] = 384;
    PIECE_TABLE_W['N'] = 64;  PIECE_TABLE_B['N'] = 448;
    PIECE_TABLE_W['B'] = 128; PIECE_TABLE_B['B'] = 512;
    PIECE_TABLE_W['R'] = 192; PIECE_TABLE_B['R'] = 576;
    PIECE_TABLE_W['Q'] = 256; PIECE_TABLE_B['Q'] = 640;
    PIECE_TABLE_W['K'] = 320; PIECE_TABLE_B['K'] = 704;

    PIECE_TABLE_W['p'] = 384; PIECE_TABLE_B['p'] = 0;
    PIECE_TABLE_W['n'] = 448; PIECE_TABLE_B['n'] = 64;
    PIECE_TABLE_W['b'] = 512; PIECE_TABLE_B['b'] = 128;
    PIECE_TABLE_W['r'] = 576; PIECE_TABLE_B['r'] = 192;
    PIECE_TABLE_W['q'] = 640; PIECE_TABLE_B['q'] = 256;
    PIECE_TABLE_W['k'] = 704; PIECE_TABLE_B['k'] = 320;

    Block* cur_block = nullptr;
    if (!g_blocks.empty() && g_blocks.back()->count < BLOCK_CAP) {
        cur_block = g_blocks.back();
    } else {
        cur_block = new Block();
        cur_block->count = 0;
        g_blocks.push_back(cur_block);
    }

    char line[1024];
    int file_lines = 0;
    int file_kept = 0;

    while (fgets(line, sizeof(line), f)) {
        file_lines++;
        g_total_lines++;
        if (max_samples > 0 && g_total_count >= max_samples) break;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        float score = 0.0f;
        float wdl = 0.5f;

        char* pipe1 = strchr(line, '|');
        if (pipe1) {
            char* pipe2 = strchr(pipe1 + 1, '|');
            score = (float)atof(pipe1 + 1);
            if (pipe2) {
                wdl = (float)atof(pipe2 + 1);
            }
        } else {
            // EPD format check
            if (strstr(line, "\"1-0\"")) {
                score = 400.0f;
                wdl = 1.0f;
            } else if (strstr(line, "\"0-1\"")) {
                score = -400.0f;
                wdl = 0.0f;
            } else if (strstr(line, "\"1/2-1/2\"")) {
                score = 0.0f;
                wdl = 0.5f;
            }
        }

        char* ptr = line;
        while (*ptr == ' ') ptr++;
        char* board_start = ptr;
        while (*ptr && *ptr != ' ' && *ptr != '|') ptr++;
        int board_len = (int)(ptr - board_start);

        while (*ptr == ' ') ptr++;
        char stm_char = *ptr;
        uint8_t stm = (stm_char == 'w') ? 0 : 1;

        // Normalized FEN for deduplication (board + stm + castling + ep + hmc when present)
        char* norm_end = ptr;
        char* hmc_start = nullptr;
        int spaces = 0;
        while (*norm_end && *norm_end != '|' && *norm_end != '\n' && *norm_end != '\r') {
            if (*norm_end == ' ') {
                while (*norm_end == ' ') norm_end++;
                if (!*norm_end || *norm_end == '|' || *norm_end == '\n' || *norm_end == '\r') break;
                spaces++;
                if (spaces == 3) {
                    hmc_start = norm_end;
                } else if (spaces >= 4) {
                    break;
                }
                continue;
            }
            norm_end++;
        }
        while (norm_end > board_start && norm_end[-1] == ' ') norm_end--;
        int hmc_val = hmc_start ? atoi(hmc_start) : 0;
        if (hmc_val < 0) hmc_val = 0;
        if (hmc_val > 255) hmc_val = 255;

        uint64_t h = hash64(board_start, (int)(norm_end - board_start));
        if (!insert_hash(h)) {
            continue; // Duplicate
        }

        if (cur_block->count >= BLOCK_CAP) {
            cur_block = new Block();
            cur_block->count = 0;
            g_blocks.push_back(cur_block);
        }

        int idx = cur_block->count;
        int16_t* cur_w = cur_block->w[idx];
        int16_t* cur_b = cur_block->b[idx];

        for (int i = 0; i < 32; ++i) {
            cur_w[i] = -1;
            cur_b[i] = -1;
        }

        int rank = 7, file = 0;
        int nw = 0, nb = 0;
        for (int i = 0; i < board_len; ++i) {
            unsigned char c = (unsigned char)board_start[i];
            if (c == '/') {
                rank--;
                file = 0;
            } else if (c >= '1' && c <= '8') {
                file += (c - '0');
            } else {
                int sq = (rank << 3) | file;
                if (nw < 32) cur_w[nw++] = (int16_t)(PIECE_TABLE_W[c] + sq);
                if (nb < 32) cur_b[nb++] = (int16_t)(PIECE_TABLE_B[c] + (sq ^ 56));
                file++;
            }
        }

        float score_stm = (stm == 0) ? score : -score;
        float wdl_stm = (stm == 0) ? wdl : (1.0f - wdl);
        float scaled = score_stm / 400.0f;
        if (scaled < -15.0f) scaled = -15.0f;
        if (scaled > 15.0f) scaled = 15.0f;
        float p_score = 1.0f / (1.0f + expf(-scaled));
        float target = (1.0f - lambda_wdl) * p_score + lambda_wdl * wdl_stm;

        cur_block->stm[idx] = stm;
        int pc_count = nw;
        if (pc_count < 2) pc_count = 2;
        if (pc_count > 32) pc_count = 32;
        cur_block->bucket[idx] = (uint8_t)pc_count;
        cur_block->hmc[idx] = (uint8_t)hmc_val;
        cur_block->tgt[idx] = target;
        cur_block->wdl[idx] = wdl_stm;
        cur_block->cp[idx] = score_stm;
        cur_block->count++;
        g_total_count++;
        file_kept++;
    }

    fclose(f);
    if (out_file_lines) *out_file_lines = file_lines;
    if (out_file_kept) *out_file_kept = file_kept;
    return g_total_count;
}

int fast_loader_get_total_count() {
    return g_total_count;
}

int fast_loader_get_total_lines() {
    return g_total_lines;
}

void fast_loader_copy_to_numpy(int16_t* out_w, int16_t* out_b, uint8_t* out_stm, uint8_t* out_bucket, uint8_t* out_hmc, float* out_tgt, float* out_wdl, float* out_cp) {
    // 1. Free deduplication hash table immediately to release 1-2 GB of RAM before copying
    if (g_hash_table) {
        free(g_hash_table);
        g_hash_table = nullptr;
        g_hash_mask = 0;
        g_hash_count = 0;
    }

    // 2. Stream copy block by block, freeing each block immediately to avoid memory duplication
    size_t offset = 0;
    for (size_t i = 0; i < g_blocks.size(); ++i) {
        Block* b = g_blocks[i];
        if (b) {
            int cnt = b->count;
            if (cnt > 0) {
                memcpy(out_w + offset * 32, b->w, (size_t)cnt * 32 * sizeof(int16_t));
                memcpy(out_b + offset * 32, b->b, (size_t)cnt * 32 * sizeof(int16_t));
                memcpy(out_stm + offset, b->stm, (size_t)cnt * sizeof(uint8_t));
                memcpy(out_bucket + offset, b->bucket, (size_t)cnt * sizeof(uint8_t));
                memcpy(out_hmc + offset, b->hmc, (size_t)cnt * sizeof(uint8_t));
                memcpy(out_tgt + offset, b->tgt, (size_t)cnt * sizeof(float));
                memcpy(out_wdl + offset, b->wdl, (size_t)cnt * sizeof(float));
                memcpy(out_cp + offset, b->cp, (size_t)cnt * sizeof(float));
                offset += cnt;
            }
            delete b;
            g_blocks[i] = nullptr;
        }
    }
    g_blocks.clear();
}

void fast_loader_cleanup() {
    for (Block* b : g_blocks) {
        if (b) delete b;
    }
    g_blocks.clear();
    if (g_hash_table) {
        free(g_hash_table);
        g_hash_table = nullptr;
    }
    g_hash_mask = 0;
    g_hash_count = 0;
    g_total_count = 0;
    g_total_lines = 0;
}

}
'''

def get_compiled_fast_loader():
    """
    Attempts to compile and load the high-speed C++ dataset parser shared library.
    Returns CDLL instance if successful, or None if compilation/loading fails.
    """
    compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not compiler:
        return None

    # Safe base directory resolution that handles Jupyter / Kaggle interactive cells (where __file__ is undefined)
    base_dir = Path(__file__).resolve().parent if ("__file__" in globals() and __file__) else Path.cwd()

    candidates = [
        Path("/kaggle/working/.cache"),
        base_dir / ".cache",
        Path.cwd() / ".cache",
        Path(tempfile.gettempdir()) / "gc_nnue_cache"
    ]
    cache_dir = None
    for cand in candidates:
        try:
            cand.mkdir(parents=True, exist_ok=True)
            test_file = cand / ".write_test"
            test_file.write_text("test")
            test_file.unlink()
            cache_dir = cand
            break
        except Exception:
            continue

    if cache_dir is None:
        return None

    suffix = ".dll" if sys.platform.startswith("win") else ".so"
    so_path = cache_dir / f"gc_fast_loader{suffix}"
    cpp_path = cache_dir / "gc_fast_loader.cpp"

    needs_build = not so_path.exists()
    if not needs_build:
        try:
            with open(cpp_path, "r", encoding="utf-8") as f:
                if f.read() != CPP_FAST_LOADER_SOURCE:
                    needs_build = True
        except Exception:
            needs_build = True

    if needs_build:
        try:
            print("[*] Compiling high-speed C++ dataset parser extension...")
            with open(cpp_path, "w", encoding="utf-8") as f:
                f.write(CPP_FAST_LOADER_SOURCE)

            flags = ["-O3", "-shared", "-fPIC"]
            if sys.platform.startswith("win"):
                flags.extend(["-static", "-static-libgcc", "-static-libstdc++"])
            cmd = [compiler] + flags + [str(cpp_path), "-o", str(so_path)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                print(f"[WARN] C++ compiler error:\n{proc.stderr.strip()}")
                return None
            print(f"[+] High-speed C++ parser extension ready ({so_path.name})")
        except Exception as e:
            print(f"[WARN] Failed to compile C++ fast loader ({e}), falling back to Python parser.")
            return None

    try:
        lib = ctypes.CDLL(str(so_path.resolve()))
        lib.fast_loader_reset.argtypes = [ctypes.c_int]
        lib.fast_loader_read_file.argtypes = [
            ctypes.c_char_p,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int)
        ]
        lib.fast_loader_read_file.restype = ctypes.c_int
        lib.fast_loader_get_total_count.restype = ctypes.c_int
        lib.fast_loader_get_total_lines.restype = ctypes.c_int
        lib.fast_loader_copy_to_numpy.argtypes = [
            ctypes.POINTER(ctypes.c_int16),
            ctypes.POINTER(ctypes.c_int16),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
        ]
        lib.fast_loader_cleanup.argtypes = []
        return lib
    except Exception as e:
        print(f"[WARN] Failed to load C++ extension ({e}), falling back to Python parser.")
        return None


def load_dataset(data_path=None, max_samples=None, lambda_wdl=0.5, log_interval=100000):
    """
    Reads dataset file(s) with FEN deduplication and converts to tensor arrays.
    Automatically uses high-speed compiled C++ extension (>1,000,000 lines/s)
    with graceful fallback to optimized Python chunked loader.
    """
    files_to_read = resolve_dataset_files(data_path)
    if not files_to_read:
        raise FileNotFoundError(f"No valid dataset file(s) found at: {data_path}")

    total_bytes = sum(f.stat().st_size for f in files_to_read)
    total_mb = total_bytes / (1024 * 1024)
    print("=" * 75)
    print(f"[*] Dataset Discovery: Found {len(files_to_read)} file(s) ({total_mb:.1f} MB total)")
    for idx, fpath in enumerate(files_to_read, 1):
        fsize_mb = fpath.stat().st_size / (1024 * 1024)
        print(f"    [{idx}/{len(files_to_read)}] {fpath} ({fsize_mb:.2f} MB)")
    print("=" * 75)

    has_gz = any(str(f).endswith(".gz") for f in files_to_read)
    fast_loader = None
    if not has_gz:
        fast_loader = get_compiled_fast_loader()

    # --- Mode 1: High-Speed C++ Dataset Loader ---
    if fast_loader is not None:
        print("[*] Engine Mode: High-Speed C++ Dataset Loader (>1,000,000 lines/s)")
        reserve_hint = max_samples if max_samples is not None and max_samples > 0 else int(total_bytes / 70)
        fast_loader.fast_loader_reset(reserve_hint)

        start_time = time.time()
        for idx, fpath in enumerate(files_to_read, 1):
            if max_samples and fast_loader.fast_loader_get_total_count() >= max_samples:
                break
            out_lines = ctypes.c_int(0)
            out_kept = ctypes.c_int(0)
            t_f0 = time.time()
            rem = (max_samples - fast_loader.fast_loader_get_total_count()) if max_samples else 0

            fast_loader.fast_loader_read_file(
                str(fpath.resolve()).encode('utf-8'),
                ctypes.c_float(lambda_wdl),
                rem if max_samples else 0,
                ctypes.byref(out_lines),
                ctypes.byref(out_kept)
            )
            t_fe = time.time() - t_f0
            rate = out_lines.value / t_fe if t_fe > 0 else 0
            print(f"  [{idx}/{len(files_to_read)}] {fpath.name}: {out_lines.value:,} lines -> {out_kept.value:,} unique "
                  f"in {t_fe:.2f}s ({rate:,.0f} lines/s)")

        count = fast_loader.fast_loader_get_total_count()
        total_lines = fast_loader.fast_loader_get_total_lines()

        # Allocate exact numpy arrays and copy in one bulk operation
        w_arr = np.empty((count, 32), dtype=np.int16)
        b_arr = np.empty((count, 32), dtype=np.int16)
        stm_arr = np.empty(count, dtype=np.uint8)
        bucket_arr = np.empty(count, dtype=np.uint8)
        hmc_arr = np.empty(count, dtype=np.uint8)
        tgt_arr = np.empty(count, dtype=np.float32)
        wdl_arr = np.empty(count, dtype=np.float32)
        cp_arr = np.empty(count, dtype=np.float32)

        fast_loader.fast_loader_copy_to_numpy(
            w_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
            b_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
            stm_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            bucket_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            hmc_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            tgt_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            wdl_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            cp_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        )
        fast_loader.fast_loader_cleanup()

        elapsed = time.time() - start_time
        dup_count = total_lines - count
        overall_rate = total_lines / elapsed if elapsed > 0 else 0
        mem_mb = (w_arr.nbytes + b_arr.nbytes + stm_arr.nbytes + bucket_arr.nbytes +
                  hmc_arr.nbytes + tgt_arr.nbytes + wdl_arr.nbytes + cp_arr.nbytes) / (1024 * 1024)
        print(f"Dataset loaded: {count:,} unique positions from {total_lines:,} lines "
              f"({dup_count:,} duplicates filtered) in {elapsed:.2f}s ({overall_rate:,.0f} lines/s)")
        print(f"[*] Memory Footprint: {mem_mb:.1f} MB (compact int16 layout, zero memory duplication)")

        w_tensor = torch.from_numpy(w_arr)
        b_tensor = torch.from_numpy(b_arr)
        stm_tensor = torch.from_numpy(stm_arr)
        tgt_tensor = torch.from_numpy(tgt_arr)
        bucket_tensor = torch.from_numpy(bucket_arr)
        hmc_tensor = torch.from_numpy(hmc_arr)
        wdl_tensor = torch.from_numpy(wdl_arr)
        cp_tensor = torch.from_numpy(cp_arr)

        import gc
        gc.collect()
        return ChessNNUEDataset(w_tensor, b_tensor, stm_tensor, tgt_tensor, bucket_tensor,
                                hmc=hmc_tensor, wdl=wdl_tensor, cp=cp_tensor)

    # --- Mode 2: Optimized Python Chunked Loader (Fallback) ---
    print("[*] Engine Mode: Python Chunked Dataset Loader (Fallback)")
    chunk_size = 100000
    w_chunks = []
    b_chunks = []
    stm_chunks = []
    bucket_chunks = []
    hmc_chunks = []
    tgt_chunks = []
    wdl_chunks = []
    cp_chunks = []

    cur_w = np.full((chunk_size, 32), -1, dtype=np.int16)
    cur_b = np.full((chunk_size, 32), -1, dtype=np.int16)
    cur_stm = np.zeros(chunk_size, dtype=np.uint8)
    cur_bucket = np.zeros(chunk_size, dtype=np.uint8)
    cur_hmc = np.zeros(chunk_size, dtype=np.uint8)
    cur_tgt = np.zeros(chunk_size, dtype=np.float32)
    cur_wdl = np.zeros(chunk_size, dtype=np.float32)
    cur_cp = np.zeros(chunk_size, dtype=np.float32)
    cur_count = 0

    seen_hashes = set()
    total_lines = 0
    total_kept = 0
    start_time = time.time()

    for fpath in files_to_read:
        if max_samples and total_kept >= max_samples:
            break
        with open_file_stream(fpath) as f:
            for line in f:
                total_lines += 1
                parsed = parse_data_line(line, lambda_wdl=lambda_wdl)
                if parsed is None:
                    continue

                w_idx, b_idx, stm, target, norm_fen, bucket, hmc, wdl_stm, score_stm = parsed

                h = zlib.crc32(norm_fen.encode('ascii'))
                if h in seen_hashes:
                    continue
                seen_hashes.add(h)

                if cur_count >= chunk_size:
                    w_chunks.append(cur_w)
                    b_chunks.append(cur_b)
                    stm_chunks.append(cur_stm)
                    bucket_chunks.append(cur_bucket)
                    hmc_chunks.append(cur_hmc)
                    tgt_chunks.append(cur_tgt)
                    wdl_chunks.append(cur_wdl)
                    cp_chunks.append(cur_cp)
                    cur_w = np.full((chunk_size, 32), -1, dtype=np.int16)
                    cur_b = np.full((chunk_size, 32), -1, dtype=np.int16)
                    cur_stm = np.zeros(chunk_size, dtype=np.uint8)
                    cur_bucket = np.zeros(chunk_size, dtype=np.uint8)
                    cur_hmc = np.zeros(chunk_size, dtype=np.uint8)
                    cur_tgt = np.zeros(chunk_size, dtype=np.float32)
                    cur_wdl = np.zeros(chunk_size, dtype=np.float32)
                    cur_cp = np.zeros(chunk_size, dtype=np.float32)
                    cur_count = 0

                nw = min(len(w_idx), 32)
                nb = min(len(b_idx), 32)
                cur_w[cur_count, :nw] = w_idx[:nw]
                cur_b[cur_count, :nb] = b_idx[:nb]
                cur_stm[cur_count] = stm
                cur_bucket[cur_count] = bucket
                cur_hmc[cur_count] = hmc
                cur_tgt[cur_count] = target
                cur_wdl[cur_count] = wdl_stm
                cur_cp[cur_count] = score_stm
                cur_count += 1
                total_kept += 1

                if max_samples is not None and total_kept >= max_samples:
                    break

                if total_lines % log_interval == 0:
                    elapsed = time.time() - start_time
                    rate = total_lines / elapsed if elapsed > 0 else 0
                    print(f"  Processed {total_lines:,} lines | Unique kept: {total_kept:,} ({rate:.0f} lines/s)")

    if cur_count > 0:
        w_chunks.append(cur_w[:cur_count])
        b_chunks.append(cur_b[:cur_count])
        stm_chunks.append(cur_stm[:cur_count])
        bucket_chunks.append(cur_bucket[:cur_count])
        hmc_chunks.append(cur_hmc[:cur_count])
        tgt_chunks.append(cur_tgt[:cur_count])
        wdl_chunks.append(cur_wdl[:cur_count])
        cp_chunks.append(cur_cp[:cur_count])

    elapsed = time.time() - start_time
    dup_count = total_lines - total_kept
    overall_rate = total_lines / elapsed if elapsed > 0 else 0
    print(f"Dataset loaded: {total_kept:,} unique positions from {total_lines:,} lines "
          f"({dup_count:,} duplicates filtered) in {elapsed:.2f}s ({overall_rate:,.0f} lines/s)")

    if not w_chunks:
        return ChessNNUEDataset(torch.empty((0, 32), dtype=torch.int16),
                                torch.empty((0, 32), dtype=torch.int16),
                                torch.empty(0, dtype=torch.uint8),
                                torch.empty(0, dtype=torch.float32),
                                torch.empty(0, dtype=torch.uint8))

    w_arr = np.concatenate(w_chunks, axis=0)
    b_arr = np.concatenate(b_chunks, axis=0)
    stm_arr = np.concatenate(stm_chunks, axis=0)
    bucket_arr = np.concatenate(bucket_chunks, axis=0)
    hmc_arr = np.concatenate(hmc_chunks, axis=0)
    tgt_arr = np.concatenate(tgt_chunks, axis=0)
    wdl_arr = np.concatenate(wdl_chunks, axis=0)
    cp_arr = np.concatenate(cp_chunks, axis=0)
    del w_chunks, b_chunks, stm_chunks, bucket_chunks, hmc_chunks, tgt_chunks, wdl_chunks, cp_chunks

    import gc
    gc.collect()
    return ChessNNUEDataset(
        torch.from_numpy(w_arr),
        torch.from_numpy(b_arr),
        torch.from_numpy(stm_arr),
        torch.from_numpy(tgt_arr),
        torch.from_numpy(bucket_arr),
        hmc=torch.from_numpy(hmc_arr),
        wdl=torch.from_numpy(wdl_arr),
        cp=torch.from_numpy(cp_arr),
    )


# ==============================================================================
# NNUE PyTorch Architecture
# ==============================================================================

def compute_cphg_features(s, n):
    """Computes 3-Zone Cross-Perspective Hadamard-Gated activations (B, 1024) from clamped [0,1] accumulators."""
    s1, n1 = s[:, :256], n[:, :256]
    s2, n2 = s[:, 256:384], n[:, 256:384]
    s3, n3 = s[:, 384:512], n[:, 384:512]
    diff2 = s2 - n2
    s3_nb = s3.view(-1, 64, 2).flip(-1).reshape(-1, 128)
    n3_nb = n3.view(-1, 64, 2).flip(-1).reshape(-1, 128)
    return torch.cat([
        s1 * s1, s2 * n2, s3 * n3_nb,
        n1 * n1, diff2 * diff2, n3 * s3_nb,
    ], dim=-1)


def compute_relgeo_packed_batch(bw, bstm, bhmc, device):
    """
    Computes exact RelGeo packed 1D indices + offsets on GPU/CPU directly from (B, 32)
    White-POV 768 indices, (B,) stm, and (B,) hmc without duplicating dataset RAM.
    """
    B = bw.shape[0]
    w = bw.to(device=device, dtype=torch.long)
    stm = bstm.to(device=device, dtype=torch.long)
    hmc = bhmc.to(device=device, dtype=torch.long)

    valid = w >= 0
    w_safe = torch.where(valid, w, torch.zeros_like(w))
    c = w_safe // 384
    rem = w_safe % 384
    pt = (rem // 64) + 1
    sq = rem % 64

    is_stm = c == stm.unsqueeze(1)
    is_ok = valid & is_stm & (pt == 6)
    is_ek = valid & (~is_stm) & (pt == 6)

    ok_raw = (sq * is_ok.long()).sum(dim=1)
    ek_raw = (sq * is_ek.long()).sum(dim=1)

    flip_v = stm * 56
    wk_persp = ok_raw ^ flip_v
    mirror_h = ((wk_persp & 4) == 0).long() * 7
    flip_all = flip_v ^ mirror_h

    ok_sq = ok_raw ^ flip_all
    ek_sq = ek_raw ^ flip_all
    ok_f, ok_r = ok_sq & 7, ok_sq >> 3
    ek_f, ek_r = ek_sq & 7, ek_sq >> 3

    canon_sq = sq ^ flip_all.unsqueeze(1)
    pf, pr = canon_sq & 7, canon_sq >> 3

    out = torch.full((B, 51), -1, dtype=torch.long, device=device)
    out[:, 0] = (ek_r - ok_r + 7) * 15 + (ek_f - ok_f + 7)
    out[:, 1] = 225 + ok_sq
    out[:, 2] = 289 + ek_sq

    non_king = valid & (pt != 6)
    c_rel = (~is_stm).long()
    nk_idx = 353 + (c_rel * 5 + (pt - 1)) * 225 + (pr - ek_r.unsqueeze(1) + 7) * 15 + (pf - ek_f.unsqueeze(1) + 7)
    out[:, 3:35] = torch.where(non_king, nk_idx, torch.full_like(nk_idx, -1))

    is_light = ((pf + pr) & 1) != 0
    is_stm_b = valid & is_stm & (pt == 3)
    is_nstm_b = valid & (~is_stm) & (pt == 3)
    stm_light_b = (is_stm_b & is_light).any(dim=1)
    stm_dark_b = (is_stm_b & (~is_light)).any(dim=1)
    ek_light = (((ek_f + ek_r) & 1) != 0)

    has_other = (valid & ((pt == 2) | (pt == 4) | (pt == 5))).any(dim=1)
    pure_ocb = (~has_other) & (is_stm_b.sum(dim=1) == 1) & (is_nstm_b.sum(dim=1) == 1) & (
        ((is_stm_b & is_light).any(dim=1)) != ((is_nstm_b & is_light).any(dim=1))
    )

    out[:, 35] = torch.where(stm_light_b, torch.full((B,), 2603, device=device, dtype=torch.long), -1)
    out[:, 36] = torch.where(stm_dark_b, torch.full((B,), 2604, device=device, dtype=torch.long), -1)
    out[:, 37] = torch.where(ek_light, torch.full((B,), 2605, device=device, dtype=torch.long), -1)
    out[:, 38] = torch.where(pure_ocb, torch.full((B,), 2606, device=device, dtype=torch.long), -1)

    thresh_t = torch.tensor(HMC_THRESH, dtype=torch.long, device=device).unsqueeze(0)
    hmc_mask = hmc.unsqueeze(1) >= thresh_t
    hmc_ids = torch.arange(2607, 2619, dtype=torch.long, device=device).unsqueeze(0).expand(B, -1)
    out[:, 39:51] = torch.where(hmc_mask, hmc_ids, -1)

    valid_rel = out >= 0
    flat_idx = out[valid_rel]
    offsets = torch.empty(B, dtype=torch.long, device=device)
    offsets[0] = 0
    if B > 1:
        offsets[1:] = valid_rel.sum(dim=1).cumsum(0)[:-1]
    return flat_idx, offsets


class PerspectiveNNUE(nn.Module):
    """
    GCNNUE04 Unified Cross-Perspective Hadamard-Gated NNUE (CP-HG).
    """
    def __init__(self, num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS):
        super().__init__()
        self.num_features = num_features
        self.hidden_size = hidden_size
        self.output_buckets = output_buckets

        self.ft = nn.Linear(num_features, hidden_size)
        self.out_weights = nn.Parameter(torch.empty(output_buckets, hidden_size * 2))
        self.out_biases = nn.Parameter(torch.empty(output_buckets))
        self._init_weights()

    def _init_weights(self):
        nn.init.normal_(self.ft.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.ft.bias, 0.1)
        nn.init.normal_(self.out_weights, mean=0.0, std=0.02)
        nn.init.constant_(self.out_biases, 0.0)

    def forward(self, feat_w, feat_b, stm, buckets=None, offsets_w=None, offsets_b=None):
        if offsets_w is not None and offsets_b is not None:
            ft_w_t = self.ft.weight.t()
            acc_w = F.embedding_bag(feat_w.long(), ft_w_t, offsets_w, mode="sum") + self.ft.bias
            acc_b = F.embedding_bag(feat_b.long(), ft_w_t, offsets_b, mode="sum") + self.ft.bias
        elif feat_w.dtype in (torch.int16, torch.int32, torch.int64):
            w_pad = self.ft.weight.new_zeros(1, self.hidden_size)
            ft_table = torch.cat([self.ft.weight.t(), w_pad], dim=0)
            idx_w = feat_w.long()
            idx_b = feat_b.long()
            if (idx_w < 0).any():
                idx_w = idx_w.masked_fill(idx_w < 0, self.num_features)
            if (idx_b < 0).any():
                idx_b = idx_b.masked_fill(idx_b < 0, self.num_features)
            acc_w = F.embedding_bag(idx_w, ft_table, mode="sum", padding_idx=self.num_features) + self.ft.bias
            acc_b = F.embedding_bag(idx_b, ft_table, mode="sum", padding_idx=self.num_features) + self.ft.bias
        else:
            acc_w = self.ft(feat_w)
            acc_b = self.ft(feat_b)

        c_w = torch.clamp(acc_w, 0.0, 1.0)
        c_b = torch.clamp(acc_b, 0.0, 1.0)
        stm_mask = (stm.unsqueeze(-1) == 0)
        s = torch.where(stm_mask, c_w, c_b)
        n = torch.where(stm_mask, c_b, c_w)
        combined = compute_cphg_features(s, n)

        if buckets is None:
            w = self.out_weights[7].unsqueeze(0)
            b = self.out_biases[7]
            logits = (combined * w).sum(dim=-1) + b
        else:
            p = torch.clamp(buckets.long(), 2, 32) - 1
            b0 = torch.clamp(p // 4, 0, 7)
            rem = p % 4
            b1 = torch.clamp(b0 + 1, 0, 7)
            alpha = (rem.to(combined.dtype) / 4.0)

            out0 = (combined * self.out_weights[b0]).sum(dim=-1) + self.out_biases[b0]
            out1 = (combined * self.out_weights[b1]).sum(dim=-1) + self.out_biases[b1]
            logits = (1.0 - alpha) * out0 + alpha * out1

        return logits

    def evaluate_cp(self, feat_w, feat_b, stm, buckets=None):
        """Evaluates position in centipawns."""
        with torch.no_grad():
            return self.forward(feat_w, feat_b, stm, buckets) * SCALE


class GCNNUE05Net(nn.Module):
    """Original Dual-Stream CP-HG + RelGeo + Dual-Head Value*(1-DrawGate) NNUE (GCNNUE05)."""

    def __init__(self):
        super().__init__()
        self.ft = nn.Linear(NUM_FEATURES, HIDDEN_SIZE)
        self.rel_ft = nn.Linear(RELGEO_FEATURES, RELGEO_HIDDEN)
        self.mlp1 = nn.Linear(MLP_INPUT_SIZE, MLP_HIDDEN)
        self.skip_out = nn.Parameter(torch.empty(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE))
        self.mlp_out_v = nn.Parameter(torch.empty(OUTPUT_BUCKETS, MLP_HIDDEN))
        self.out_v_biases = nn.Parameter(torch.empty(OUTPUT_BUCKETS))
        self.mlp_out_d = nn.Parameter(torch.empty(OUTPUT_BUCKETS, MLP_HIDDEN))
        self.out_d_biases = nn.Parameter(torch.empty(OUTPUT_BUCKETS))
        self._init_weights()

    def _init_weights(self):
        nn.init.normal_(self.ft.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.ft.bias, 0.1)
        nn.init.normal_(self.rel_ft.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.rel_ft.bias, 0.05)
        nn.init.normal_(self.mlp1.weight, mean=0.0, std=0.04)
        nn.init.constant_(self.mlp1.bias, 0.0)
        nn.init.normal_(self.skip_out, mean=0.0, std=0.02)
        nn.init.normal_(self.mlp_out_v, mean=0.0, std=0.04)
        nn.init.constant_(self.out_v_biases, 0.0)
        nn.init.normal_(self.mlp_out_d, mean=0.0, std=0.04)
        nn.init.constant_(self.out_d_biases, -2.5)  # Initialize DrawGate low (~0.075)

    def forward_components(self, feat_w, feat_b, stm, rel_idx, piece_counts,
                           off_w=None, off_b=None, off_rel=None):
        ft_w_t = self.ft.weight.t()
        if off_w is not None:
            acc_w = F.embedding_bag(feat_w.long(), ft_w_t, off_w, mode="sum") + self.ft.bias
            acc_b = F.embedding_bag(feat_b.long(), ft_w_t, off_b, mode="sum") + self.ft.bias
        else:
            w_pad = self.ft.weight.new_zeros(1, HIDDEN_SIZE)
            ft_table = torch.cat([ft_w_t, w_pad], dim=0)
            idx_w = feat_w.long()
            idx_b = feat_b.long()
            if (idx_w < 0).any():
                idx_w = idx_w.masked_fill(idx_w < 0, NUM_FEATURES)
            if (idx_b < 0).any():
                idx_b = idx_b.masked_fill(idx_b < 0, NUM_FEATURES)
            acc_w = F.embedding_bag(idx_w, ft_table, mode="sum", padding_idx=NUM_FEATURES) + self.ft.bias
            acc_b = F.embedding_bag(idx_b, ft_table, mode="sum", padding_idx=NUM_FEATURES) + self.ft.bias

        c_w = torch.clamp(acc_w, 0.0, 1.0)
        c_b = torch.clamp(acc_b, 0.0, 1.0)
        stm_mask = (stm.unsqueeze(-1) == 0)
        s = torch.where(stm_mask, c_w, c_b)
        n = torch.where(stm_mask, c_b, c_w)
        a_cphg = compute_cphg_features(s, n)

        rel_w_t = self.rel_ft.weight.t()
        if off_rel is not None:
            rel_acc = F.embedding_bag(rel_idx.long(), rel_w_t, off_rel, mode="sum") + self.rel_ft.bias
        else:
            r_pad = self.rel_ft.weight.new_zeros(1, RELGEO_HIDDEN)
            r_table = torch.cat([rel_w_t, r_pad], dim=0)
            rel_acc = F.embedding_bag(rel_idx.long(), r_table, mode="sum", padding_idx=RELGEO_FEATURES) + self.rel_ft.bias
        r_act = torch.clamp(rel_acc, 0.0, 1.0) ** 2

        fusion_in = torch.cat([a_cphg, r_act], dim=-1)
        w1 = torch.clamp(self.mlp1.weight, -127.0 / QB, 127.0 / QB)
        z1 = F.linear(fusion_in, w1, self.mlp1.bias)
        h32 = torch.clamp(z1, 0.0, 1.0) ** 2

        p = torch.clamp(piece_counts.long(), 2, 32) - 1
        b0 = torch.clamp(p // 4, 0, 7)
        rem = p % 4
        b1 = torch.clamp(b0 + 1, 0, 7)
        alpha = rem.to(a_cphg.dtype) / 4.0

        v0 = (a_cphg * self.skip_out[b0]).sum(dim=-1) + (h32 * self.mlp_out_v[b0]).sum(dim=-1) + self.out_v_biases[b0]
        v1 = (a_cphg * self.skip_out[b1]).sum(dim=-1) + (h32 * self.mlp_out_v[b1]).sum(dim=-1) + self.out_v_biases[b1]
        v = (1.0 - alpha) * v0 + alpha * v1

        d0 = (h32 * self.mlp_out_d[b0]).sum(dim=-1) + self.out_d_biases[b0]
        d1 = (h32 * self.mlp_out_d[b1]).sum(dim=-1) + self.out_d_biases[b1]
        d_logit = (1.0 - alpha) * d0 + alpha * d1
        draw_prob = torch.sigmoid(d_logit)

        logits = v * (1.0 - draw_prob)
        return logits, v, draw_prob, d_logit

    def forward(self, feat_w, feat_b, stm, rel_idx, piece_counts, off_w=None, off_b=None, off_rel=None):
        logits, _, _, _ = self.forward_components(feat_w, feat_b, stm, rel_idx, piece_counts, off_w, off_b, off_rel)
        return logits


# ==============================================================================
# Model Quantization & Binary Exporter
# ==============================================================================

def export_quantized_binary_v5(model, output_path, qa=QA, qb=QB):
    """Quantizes floating-point GCNNUE05 weights and exports a 1,512,664-byte binary file."""
    model.eval()
    scale_bias = qa * qb
    with torch.no_grad():
        w_ft_q = torch.clamp(torch.round(model.ft.weight.data.t().cpu().float() * qa), -32768, 32767).to(torch.int16)
        b_ft_q = torch.clamp(torch.round(model.ft.bias.data.cpu().float() * qa), -32768, 32767).to(torch.int16)

        w_rel_q = torch.clamp(torch.round(model.rel_ft.weight.data.t().cpu().float() * qa), -32768, 32767).to(torch.int16)
        b_rel_q = torch.clamp(torch.round(model.rel_ft.bias.data.cpu().float() * qa), -32768, 32767).to(torch.int16)

        w_m1_q = torch.clamp(torch.round(model.mlp1.weight.data.cpu().float() * qb), -127, 127).to(torch.int8)
        b_m1_q = torch.round(model.mlp1.bias.data.cpu().float() * scale_bias).to(torch.int32)

        w_skip_q = torch.clamp(torch.round(model.skip_out.data.cpu().float() * qb), -32768, 32767).to(torch.int16)
        w_mv_q = torch.clamp(torch.round(model.mlp_out_v.data.cpu().float() * qb), -32768, 32767).to(torch.int16)
        b_ov_q = torch.round(model.out_v_biases.data.cpu().float() * scale_bias).to(torch.int32)

        w_md_q = torch.clamp(torch.round(model.mlp_out_d.data.cpu().float() * qb), -32768, 32767).to(torch.int16)
        b_od_q = torch.round(model.out_d_biases.data.cpu().float() * scale_bias).to(torch.int32)

    out_file = Path(output_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with open(out_file, "wb") as f:
        f.write(MAGIC_HEADER_V5)
        f.write(struct.pack("<4i", qa, qb, RELGEO_HIDDEN, MLP_HIDDEN))
        for arr in (w_ft_q, b_ft_q, w_rel_q, b_rel_q, w_m1_q, b_m1_q,
                    w_skip_q, w_mv_q, b_ov_q, w_md_q, b_od_q):
            f.write(arr.numpy().tobytes())

    actual_size = os.path.getsize(out_file)
    if actual_size != EXPECTED_FILE_SIZE_V5:
        raise ValueError(f"GCNNUE05 export size mismatch: {actual_size} != {EXPECTED_FILE_SIZE_V5}")

    print(f"\n[+] Successfully exported quantized GCNNUE05 NNUE model:")
    print(f"    File: {out_file.resolve()}")
    print(f"    Magic: GCNNUE05 (Pure RelGeo Dual-Stream + DrawGate)")
    print(f"    Size: {actual_size:,} bytes (verified exact)")


def export_quantized_binary(model, output_path, qa=QA, qb=QB):
    """Quantizes floating-point weights to integers and exports GCNNUE05 or GCNNUE04 binary."""
    if isinstance(model, GCNNUE05Net):
        export_quantized_binary_v5(model, output_path, qa=qa, qb=qb)
        return

    model.eval()
    with torch.no_grad():
        w_ft = model.ft.weight.data.t().cpu().float()
        w_ft_q = torch.clamp(torch.round(w_ft * qa), -32768, 32767).to(torch.int16)

        b_ft = model.ft.bias.data.cpu().float()
        b_ft_q = torch.clamp(torch.round(b_ft * qa), -32768, 32767).to(torch.int16)

        w_out = model.out_weights.data.cpu().float()
        w_out_q = torch.clamp(torch.round(w_out * qb), -32768, 32767).to(torch.int16)

        b_out = model.out_biases.data.cpu().float()
        scale_bias = qa * qb
        b_out_q = torch.round(b_out * scale_bias).to(torch.int32)

        out_file = Path(output_path)
        out_file.parent.mkdir(parents=True, exist_ok=True)

        with open(out_file, "wb") as f:
            f.write(MAGIC_HEADER_V4)
            f.write(w_ft_q.numpy().tobytes())
            f.write(b_ft_q.numpy().tobytes())
            f.write(w_out_q.numpy().tobytes())
            f.write(b_out_q.numpy().tobytes())

        actual_size = os.path.getsize(output_path)
        if actual_size != EXPECTED_FILE_SIZE_V4:
            raise ValueError(
                f"Export size error: got {actual_size} bytes, expected exactly {EXPECTED_FILE_SIZE_V4} bytes!"
            )

        print(f"\n[+] Successfully exported quantized NNUE model:")
        print(f"    File: {out_file.resolve()}")
        print(f"    Magic: {MAGIC_HEADER_V4.decode('ascii')} (Unified CP-HG Architecture)")
        print(f"    Size: {actual_size:,} bytes (verified exact)")
        print(f"    Feature Weights: int16[{NUM_FEATURES}][{HIDDEN_SIZE}] ({w_ft_q.numel() * 2:,} bytes)")
        print(f"    Feature Biases:  int16[{HIDDEN_SIZE}] ({b_ft_q.numel() * 2:,} bytes)")
        print(f"    Output Weights:  int16[{OUTPUT_BUCKETS}][{OUTPUT_INPUT_SIZE}] ({w_out_q.numel() * 2:,} bytes)")
        print(f"    Output Biases:   int32[{OUTPUT_BUCKETS}] ({b_out_q.numel() * 4:,} bytes, scaled by {scale_bias})")


def eval_v5_binary_int(filepath, fen_str):
    """Exact integer reference implementation mirroring C++ evaluate_v5."""
    with open(filepath, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC_HEADER_V5:
            raise ValueError(f"Expected GCNNUE05, got {magic}")
        qa, qb, rel_h, mlp_h = struct.unpack("<4i", f.read(16))
        assert (qa, qb, rel_h, mlp_h) == (QA, QB, RELGEO_HIDDEN, MLP_HIDDEN)

        w_ft = np.frombuffer(f.read(NUM_FEATURES * HIDDEN_SIZE * 2), dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE)
        b_ft = np.frombuffer(f.read(HIDDEN_SIZE * 2), dtype=np.int16)
        w_rel = np.frombuffer(f.read(RELGEO_FEATURES * RELGEO_HIDDEN * 2), dtype=np.int16).reshape(RELGEO_FEATURES, RELGEO_HIDDEN)
        b_rel = np.frombuffer(f.read(RELGEO_HIDDEN * 2), dtype=np.int16)
        w_m1 = np.frombuffer(f.read(MLP_HIDDEN * MLP_INPUT_SIZE), dtype=np.int8).reshape(MLP_HIDDEN, MLP_INPUT_SIZE)
        b_m1 = np.frombuffer(f.read(MLP_HIDDEN * 4), dtype=np.int32)
        w_skip = np.frombuffer(f.read(OUTPUT_BUCKETS * OUTPUT_INPUT_SIZE * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE)
        w_mv = np.frombuffer(f.read(OUTPUT_BUCKETS * MLP_HIDDEN * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, MLP_HIDDEN)
        b_ov = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32)
        w_md = np.frombuffer(f.read(OUTPUT_BUCKETS * MLP_HIDDEN * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, MLP_HIDDEN)
        b_od = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32)

    w_idx, b_idx, stm, rel_idx, pc, _ = parse_relgeo_features(fen_str)
    acc_w = b_ft.astype(np.int32) + w_ft[w_idx].astype(np.int32).sum(axis=0)
    acc_b = b_ft.astype(np.int32) + w_ft[b_idx].astype(np.int32).sum(axis=0)
    s = np.clip(acc_w if stm == 0 else acc_b, 0, QA).astype(np.int32)
    n = np.clip(acc_b if stm == 0 else acc_w, 0, QA).astype(np.int32)

    feat1152 = np.zeros(MLP_INPUT_SIZE, dtype=np.int32)
    feat1152[0:256] = (s[:256] * s[:256]) // QA
    feat1152[512:768] = (n[:256] * n[:256]) // QA
    diff = s[256:384] - n[256:384]
    feat1152[256:384] = (s[256:384] * n[256:384]) // QA
    feat1152[768:896] = (diff * diff) // QA
    s3_nb = s[384:512].reshape(64, 2)[:, ::-1].reshape(128)
    n3_nb = n[384:512].reshape(64, 2)[:, ::-1].reshape(128)
    feat1152[384:512] = (s[384:512] * n3_nb) // QA
    feat1152[896:1024] = (n[384:512] * s3_nb) // QA

    rel_acc = b_rel.astype(np.int32) + w_rel[rel_idx].astype(np.int32).sum(axis=0)
    rc = np.clip(rel_acc, 0, QA).astype(np.int32)
    feat1152[OUTPUT_INPUT_SIZE:] = (rc * rc) // QA

    z1 = (w_m1.astype(np.int32) @ feat1152) + b_m1.astype(np.int32)
    hc = np.clip(np.trunc(z1 / QB).astype(np.int32), 0, QA)
    h32 = (hc * hc) // QA

    def bucket_val(b):
        v = int(np.dot(feat1152[:OUTPUT_INPUT_SIZE].astype(np.int64), w_skip[b].astype(np.int64)))
        v += int(np.dot(h32.astype(np.int64), w_mv[b].astype(np.int64))) + int(b_ov[b])
        d = int(np.dot(h32.astype(np.int64), w_md[b].astype(np.int64))) + int(b_od[b])
        return v, d

    p = max(2, min(32, pc)) - 1
    b0 = p // 4
    rem = p % 4
    b1 = min(b0 + 1, OUTPUT_BUCKETS - 1)
    v0, d0 = bucket_val(b0)
    denom = float(QA * QB)
    if b0 != b1 and rem != 0:
        v1, d1 = bucket_val(b1)
        vb = v0 * (4 - rem) + v1 * rem
        db = d0 * (4 - rem) + d1 * rem
        denom *= 4.0
    else:
        vb, db = v0, d0

    v_cp = (vb * SCALE) / denom
    d_logit = db / denom
    draw_prob = 1.0 / (1.0 + math.exp(-d_logit))
    return int(round(v_cp * (1.0 - draw_prob)))


def verify_exported_binary(filepath, test_fen=None):
    """
    Verifies the binary file format, integrity, and computes a test evaluation
    using exact integer arithmetic matching the C++ engine implementation.
    Supports GCNNUE05, GCNNUE04, GCNNUE03, and GCNNUE02.
    """
    print(f"\n[*] Verifying binary file: {filepath}")
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"Binary file not found: {filepath}")

    size = os.path.getsize(filepath)
    if size not in (EXPECTED_FILE_SIZE_V5, EXPECTED_FILE_SIZE_V4, EXPECTED_FILE_SIZE_V3, EXPECTED_FILE_SIZE_V2):
        raise ValueError(
            f"Size mismatch: {size} not in "
            f"({EXPECTED_FILE_SIZE_V5} v5, {EXPECTED_FILE_SIZE_V4} v3/v4, {EXPECTED_FILE_SIZE_V2} v2)"
        )

    if test_fen is None:
        test_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

    with open(filepath, "rb") as f:
        magic = f.read(8)
        if magic == MAGIC_HEADER_V5:
            if size != EXPECTED_FILE_SIZE_V5:
                raise ValueError(f"GCNNUE05 size mismatch: {size} != {EXPECTED_FILE_SIZE_V5}")
            qa, qb, rel_h, mlp_h = struct.unpack("<4i", f.read(16))
            print(f"    Magic: GCNNUE05 [OK] (QA={qa}, QB={qb}, RelGeo={rel_h}, MLP={mlp_h})")
            eval_cp = eval_v5_binary_int(filepath, test_fen)
            print(f"    Test FEN: {test_fen}")
            print(f"    Eval: {eval_cp} cp")
            print("    Binary verification passed successfully!\n")
            return eval_cp

        if magic not in (MAGIC_HEADER_V4, MAGIC_HEADER_V3, MAGIC_HEADER_V2):
            raise ValueError(f"Magic header mismatch: {magic}")

        w_ft_bytes = f.read(NUM_FEATURES * HIDDEN_SIZE * 2)
        b_ft_bytes = f.read(HIDDEN_SIZE * 2)

        if magic in (MAGIC_HEADER_V4, MAGIC_HEADER_V3):
            w_out_bytes = f.read(OUTPUT_BUCKETS * OUTPUT_INPUT_SIZE * 2)
            b_out_bytes = f.read(OUTPUT_BUCKETS * 4)
            w_out = np.frombuffer(w_out_bytes, dtype=np.int16).reshape(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE)
            b_out = np.frombuffer(b_out_bytes, dtype=np.int32)
        else:
            w_out_bytes = f.read(OUTPUT_INPUT_SIZE * 2)
            b_out_bytes = f.read(4)
            single_w = np.frombuffer(w_out_bytes, dtype=np.int16)
            single_b = np.frombuffer(b_out_bytes, dtype=np.int32)[0]
            w_out = np.tile(single_w, (OUTPUT_BUCKETS, 1))
            b_out = np.full(OUTPUT_BUCKETS, single_b, dtype=np.int32)

    w_ft = np.frombuffer(w_ft_bytes, dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE)
    b_ft = np.frombuffer(b_ft_bytes, dtype=np.int16)

    assert not np.isnan(w_ft).any(), "NaN found in feature weights"
    assert not np.isnan(b_ft).any(), "NaN found in feature biases"
    assert not np.isnan(w_out).any(), "NaN found in output weights"

    print(f"    Magic: {magic.decode('ascii')} [OK]")
    print(f"    Feature Weights: min={w_ft.min()}, max={w_ft.max()}, mean={w_ft.mean():.2f}")
    print(f"    Feature Biases:  min={b_ft.min()}, max={b_ft.max()}, mean={b_ft.mean():.2f}")
    print(f"    Output Weights:  min={w_out.min()}, max={w_out.max()}, mean={w_out.mean():.2f}")
    print(f"    Output Biases:   min={b_out.min()}, max={b_out.max()}, mean={b_out.mean():.2f}")

    is_v4 = (magic == MAGIC_HEADER_V4)
    w_idx, b_idx, stm = parse_fen_to_features(test_fen)
    piece_count = len(w_idx)
    bucket = get_output_bucket(piece_count)

    acc_w = b_ft.astype(np.int32) + w_ft[w_idx].astype(np.int32).sum(axis=0)
    acc_b = b_ft.astype(np.int32) + w_ft[b_idx].astype(np.int32).sum(axis=0)

    def c_div(a, b):
        return int(a / b)

    if is_v4:
        c_w = np.clip(acc_w, 0, QA)
        c_b = np.clip(acc_b, 0, QA)
        s = c_w if stm == 0 else c_b
        n = c_b if stm == 0 else c_w

        y_stm = np.empty(HIDDEN_SIZE, dtype=np.int64)
        y_nstm = np.empty(HIDDEN_SIZE, dtype=np.int64)
        y_stm[:256] = s[:256] * s[:256]
        y_nstm[:256] = n[:256] * n[:256]
        diff = s[256:384] - n[256:384]
        y_stm[256:384] = s[256:384] * n[256:384]
        y_nstm[256:384] = diff * diff
        s_nb = s[384:512].reshape(64, 2)[:, ::-1].reshape(128)
        n_nb = n[384:512].reshape(64, 2)[:, ::-1].reshape(128)
        y_stm[384:512] = s[384:512] * n_nb
        y_nstm[384:512] = n[384:512] * s_nb

        p = max(2, min(32, piece_count)) - 1
        b0 = p // 4
        rem = p % 4
        b1 = min(b0 + 1, OUTPUT_BUCKETS - 1)

        raw0 = int(np.dot(y_stm, w_out[b0, :HIDDEN_SIZE].astype(np.int64)) +
                   np.dot(y_nstm, w_out[b0, HIDDEN_SIZE:].astype(np.int64)))
        out0 = c_div(raw0, QA) + int(b_out[b0])

        if b0 == b1 or rem == 0:
            raw = raw0
            eval_cp = c_div(out0 * SCALE, QA * QB)
        else:
            raw1 = int(np.dot(y_stm, w_out[b1, :HIDDEN_SIZE].astype(np.int64)) +
                       np.dot(y_nstm, w_out[b1, HIDDEN_SIZE:].astype(np.int64)))
            out1 = c_div(raw1, QA) + int(b_out[b1])
            raw = raw0
            blended = out0 * (4 - rem) + out1 * rem
            eval_cp = c_div(blended * SCALE, 4 * QA * QB)
    else:
        y_w = np.clip(acc_w, 0, QA) ** 2
        y_b = np.clip(acc_b, 0, QA) ** 2
        y_stm = y_w if stm == 0 else y_b
        y_nstm = y_b if stm == 0 else y_w
        bucket_w = w_out[bucket]
        bucket_b = b_out[bucket]
        raw = int(np.dot(y_stm, bucket_w[:HIDDEN_SIZE].astype(np.int64)) +
                  np.dot(y_nstm, bucket_w[HIDDEN_SIZE:].astype(np.int64)))
        term = c_div(raw, QA) + int(bucket_b)
        eval_cp = c_div(term * SCALE, QA * QB)

    print(f"    Test FEN Evaluation ({'White' if stm == 0 else 'Black'} to move, Bucket {bucket}, Pieces {piece_count}):")
    print(f"      FEN:  {test_fen}")
    print(f"      Raw:  {raw}")
    print(f"      Eval: {eval_cp} cp")
    print("    Binary verification passed successfully!\n")
    return eval_cp


def load_pretrained_binary(model, filepath, qa=QA, qb=QB):
    """
    Warm-starts GCNNUE05Net or PerspectiveNNUE from GCNNUE05/04/03/02 binary weights.
    """
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"Pretrained NNUE file not found: {filepath}")

    with open(filepath, "rb") as f:
        magic = f.read(8)
        if magic == MAGIC_HEADER_V5:
            if not isinstance(model, GCNNUE05Net):
                raise ValueError("Cannot load GCNNUE05 checkpoint into legacy GCNNUE04 model (--arch v4).")
            qa_f, qb_f, rel_h, mlp_h = struct.unpack("<4i", f.read(16))
            if (rel_h, mlp_h) != (RELGEO_HIDDEN, MLP_HIDDEN):
                raise ValueError(f"GCNNUE05 dimension mismatch: ({rel_h}, {mlp_h})")
            sb = qa_f * qb_f
            w_ft = np.frombuffer(f.read(NUM_FEATURES * HIDDEN_SIZE * 2), dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE).astype(np.float32) / qa_f
            b_ft = np.frombuffer(f.read(HIDDEN_SIZE * 2), dtype=np.int16).astype(np.float32) / qa_f
            w_rel = np.frombuffer(f.read(RELGEO_FEATURES * RELGEO_HIDDEN * 2), dtype=np.int16).reshape(RELGEO_FEATURES, RELGEO_HIDDEN).astype(np.float32) / qa_f
            b_rel = np.frombuffer(f.read(RELGEO_HIDDEN * 2), dtype=np.int16).astype(np.float32) / qa_f
            w_m1 = np.frombuffer(f.read(MLP_HIDDEN * MLP_INPUT_SIZE), dtype=np.int8).reshape(MLP_HIDDEN, MLP_INPUT_SIZE).astype(np.float32) / qb_f
            b_m1 = np.frombuffer(f.read(MLP_HIDDEN * 4), dtype=np.int32).astype(np.float32) / sb
            w_skip = np.frombuffer(f.read(OUTPUT_BUCKETS * OUTPUT_INPUT_SIZE * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE).astype(np.float32) / qb_f
            w_mv = np.frombuffer(f.read(OUTPUT_BUCKETS * MLP_HIDDEN * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, MLP_HIDDEN).astype(np.float32) / qb_f
            b_ov = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32).astype(np.float32) / sb
            w_md = np.frombuffer(f.read(OUTPUT_BUCKETS * MLP_HIDDEN * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, MLP_HIDDEN).astype(np.float32) / qb_f
            b_od = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32).astype(np.float32) / sb
            with torch.no_grad():
                model.ft.weight.copy_(torch.from_numpy(w_ft.T))
                model.ft.bias.copy_(torch.from_numpy(b_ft))
                model.rel_ft.weight.copy_(torch.from_numpy(w_rel.T))
                model.rel_ft.bias.copy_(torch.from_numpy(b_rel))
                model.mlp1.weight.copy_(torch.from_numpy(w_m1))
                model.mlp1.bias.copy_(torch.from_numpy(b_m1))
                model.skip_out.copy_(torch.from_numpy(w_skip))
                model.mlp_out_v.copy_(torch.from_numpy(w_mv))
                model.out_v_biases.copy_(torch.from_numpy(b_ov))
                model.mlp_out_d.copy_(torch.from_numpy(w_md))
                model.out_d_biases.copy_(torch.from_numpy(b_od))
            print(f"[+] Warm-started GCNNUE05 model from GCNNUE05 checkpoint: {filepath}")
            return

        if magic not in (MAGIC_HEADER_V4, MAGIC_HEADER_V3, MAGIC_HEADER_V2):
            raise ValueError(f"Unsupported magic header in {filepath}: {magic}")

        w_ft = np.frombuffer(f.read(NUM_FEATURES * HIDDEN_SIZE * 2), dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE).astype(np.float32) / qa
        b_ft = np.frombuffer(f.read(HIDDEN_SIZE * 2), dtype=np.int16).astype(np.float32) / qa

        if magic in (MAGIC_HEADER_V4, MAGIC_HEADER_V3):
            w_out = np.frombuffer(f.read(OUTPUT_BUCKETS * OUTPUT_INPUT_SIZE * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE).astype(np.float32) / qb
            b_out = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32).astype(np.float32) / (qa * qb)
        else:
            single_w = np.frombuffer(f.read(OUTPUT_INPUT_SIZE * 2), dtype=np.int16).astype(np.float32) / qb
            single_b = float(np.frombuffer(f.read(4), dtype=np.int32)[0]) / (qa * qb)
            w_out = np.tile(single_w, (OUTPUT_BUCKETS, 1))
            b_out = np.full(OUTPUT_BUCKETS, single_b, dtype=np.float32)

    if isinstance(model, GCNNUE05Net):
        if magic != MAGIC_HEADER_V4:
            raise ValueError(f"Warm-starting GCNNUE05 requires GCNNUE04/05 binary, got {magic}")
        with torch.no_grad():
            model.ft.weight.copy_(torch.from_numpy(w_ft.T))
            model.ft.bias.copy_(torch.from_numpy(b_ft))
            model.skip_out.copy_(torch.from_numpy(w_out))
            model.out_v_biases.copy_(torch.from_numpy(b_out))
        print(f"[+] Warm-started GCNNUE05 Stream A + Skip from GCNNUE04: {filepath}")
        return

    with torch.no_grad():
        if magic == MAGIC_HEADER_V4:
            model.ft.weight.copy_(torch.from_numpy(w_ft.T))
            model.ft.bias.copy_(torch.from_numpy(b_ft))
            model.out_weights.copy_(torch.from_numpy(w_out))
            model.out_biases.copy_(torch.from_numpy(b_out))
            print(f"[+] Warm-started GCNNUE04 model directly from {filepath} (GCNNUE04)")
        else:
            w_stm = w_out[:, :HIDDEN_SIZE]
            w_nstm = w_out[:, HIDDEN_SIZE:]
            importance = np.abs(w_stm).mean(axis=0) + np.abs(w_nstm).mean(axis=0)
            order = np.argsort(-importance)

            w_ft_sorted = w_ft[:, order]
            b_ft_sorted = b_ft[order]
            w_stm_sorted = w_stm[:, order].copy()
            w_nstm_sorted = w_nstm[:, order].copy()
            w_stm_sorted[:, 256:] *= 0.05
            w_nstm_sorted[:, 256:] *= 0.05
            w_out_new = np.concatenate([w_stm_sorted, w_nstm_sorted], axis=1)

            model.ft.weight.copy_(torch.from_numpy(w_ft_sorted.T))
            model.ft.bias.copy_(torch.from_numpy(b_ft_sorted))
            model.out_weights.copy_(torch.from_numpy(w_out_new))
            model.out_biases.copy_(torch.from_numpy(b_out))
            print(f"[+] Warm-started GCNNUE04 model from {filepath} ({magic.decode('ascii')} -> CP-HG Zone-Sorted)")


def apply_teacher_distillation(dataset, teacher_path, lambda_wdl, device, qa=QA, qb=QB, batch_size=16384):
    """
    Replaces dataset.targets (assumed pure WDL when --teacher is enabled) with
    (1 - lambda_wdl) * sigmoid(teacher_logit) + lambda_wdl * wdl.
    Supports GCNNUE05, GCNNUE04, GCNNUE03, and GCNNUE02 teachers.
    """
    if not os.path.isfile(teacher_path):
        raise FileNotFoundError(f"Teacher NNUE file not found: {teacher_path}")

    with open(teacher_path, "rb") as f:
        magic = f.read(8)
    if magic not in (MAGIC_HEADER_V5, MAGIC_HEADER_V4, MAGIC_HEADER_V3, MAGIC_HEADER_V2):
        raise ValueError(f"Unsupported teacher magic header in {teacher_path}: {magic}")

    if magic == MAGIC_HEADER_V5:
        teacher_model = GCNNUE05Net()
        load_pretrained_binary(teacher_model, teacher_path, qa=qa, qb=qb)
        teacher_model = teacher_model.to(device).eval()
    elif magic == MAGIC_HEADER_V4:
        teacher_model = PerspectiveNNUE(NUM_FEATURES, HIDDEN_SIZE, OUTPUT_BUCKETS)
        load_pretrained_binary(teacher_model, teacher_path, qa=qa, qb=qb)
        teacher_model = teacher_model.to(device).eval()
    else:
        with open(teacher_path, "rb") as f:
            f.read(8)
            w_ft = np.frombuffer(f.read(NUM_FEATURES * HIDDEN_SIZE * 2), dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE).astype(np.float32) / qa
            b_ft = np.frombuffer(f.read(HIDDEN_SIZE * 2), dtype=np.int16).astype(np.float32) / qa
            if magic == MAGIC_HEADER_V3:
                w_out = np.frombuffer(f.read(OUTPUT_BUCKETS * OUTPUT_INPUT_SIZE * 2), dtype=np.int16).reshape(OUTPUT_BUCKETS, OUTPUT_INPUT_SIZE).astype(np.float32) / qb
                b_out = np.frombuffer(f.read(OUTPUT_BUCKETS * 4), dtype=np.int32).astype(np.float32) / (qa * qb)
            else:
                single_w = np.frombuffer(f.read(OUTPUT_INPUT_SIZE * 2), dtype=np.int16).astype(np.float32) / qb
                single_b = float(np.frombuffer(f.read(4), dtype=np.int32)[0]) / (qa * qb)
                w_out = np.tile(single_w, (OUTPUT_BUCKETS, 1))
                b_out = np.full(OUTPUT_BUCKETS, single_b, dtype=np.float32)
        ft_w_t = torch.from_numpy(w_ft).to(device)
        ft_b_t = torch.from_numpy(b_ft).to(device)
        out_w_t = torch.from_numpy(w_out).to(device)
        out_b_t = torch.from_numpy(b_out).to(device)

    n = len(dataset)
    t0 = time.time()
    print(f"[*] Distilling soft targets from teacher {teacher_path} ({magic.decode('ascii')}) with lambda_wdl={lambda_wdl}...")
    with torch.no_grad():
        for start_idx in range(0, n, batch_size):
            end_idx = min(start_idx + batch_size, n)
            cur_b = end_idx - start_idx
            bw = dataset.w_indices[start_idx:end_idx]
            bb = dataset.b_indices[start_idx:end_idx]
            valid_w = (bw >= 0)
            valid_b = (bb >= 0)
            feat_w = bw[valid_w].to(device=device, dtype=torch.long)
            feat_b = bb[valid_b].to(device=device, dtype=torch.long)
            off_w = torch.empty(cur_b, dtype=torch.long, device=device)
            off_b = torch.empty(cur_b, dtype=torch.long, device=device)
            off_w[0] = 0
            off_b[0] = 0
            if cur_b > 1:
                off_w[1:] = valid_w.sum(dim=1).cumsum(0)[:-1].to(device=device)
                off_b[1:] = valid_b.sum(dim=1).cumsum(0)[:-1].to(device=device)
            bstm = dataset.stm[start_idx:end_idx].to(device=device, dtype=torch.long)
            bbucket = dataset.buckets[start_idx:end_idx].to(device=device, dtype=torch.long)

            if magic == MAGIC_HEADER_V5:
                bhmc = dataset.hmc[start_idx:end_idx].to(device=device, dtype=torch.long)
                frel, orel = compute_relgeo_packed_batch(bw, bstm, bhmc, device)
                logits = teacher_model(feat_w, feat_b, bstm, frel, bbucket, off_w, off_b, orel)
            elif magic == MAGIC_HEADER_V4:
                logits = teacher_model(feat_w, feat_b, bstm, bbucket, off_w, off_b)
            else:
                acc_w = torch.clamp(F.embedding_bag(feat_w, ft_w_t, off_w, mode="sum") + ft_b_t, 0.0, 1.0)
                acc_b = torch.clamp(F.embedding_bag(feat_b, ft_w_t, off_b, mode="sum") + ft_b_t, 0.0, 1.0)
                y_w = acc_w * acc_w
                y_b = acc_b * acc_b
                stm_mask = (bstm.unsqueeze(-1) == 0)
                y_stm = torch.where(stm_mask, y_w, y_b)
                y_nstm = torch.where(stm_mask, y_b, y_w)
                combined = torch.cat([y_stm, y_nstm], dim=-1)
                b_idx = torch.clamp((bbucket - 1) // 4, 0, OUTPUT_BUCKETS - 1)
                logits = (combined * out_w_t[b_idx]).sum(dim=-1) + out_b_t[b_idx]

            p_teacher = torch.sigmoid(logits).cpu()
            wdl = dataset.targets[start_idx:end_idx]
            dataset.targets[start_idx:end_idx] = (1.0 - lambda_wdl) * p_teacher + lambda_wdl * wdl

    print(f"[+] Teacher soft targets computed across {n:,} positions in {time.time() - t0:.2f}s")


def export_dummy_model(output_path, arch="v5", verify=True, test_fen=None, seed=42):
    """Creates an initialized dummy GCNNUE05 or GCNNUE04 model and exports it."""
    print(f"\n[*] Generating dummy {arch.upper()} NNUE model (seed={seed})...")
    torch.manual_seed(seed)
    np.random.seed(seed)
    model = GCNNUE05Net() if arch == "v5" else PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS)
    export_quantized_binary(model, output_path)
    if verify:
        verify_exported_binary(output_path, test_fen=test_fen)


def test_parity(engine_path, out_nnue=None):
    """Exports a deterministic GCNNUE05 test network and verifies Python-int vs C++ parity."""
    cleanup_temp = False
    if not out_nnue:
        fd, out_nnue = tempfile.mkstemp(suffix=".nnue", prefix="gc_parity_v5_")
        os.close(fd)
        cleanup_temp = True

    try:
        torch.manual_seed(42)
        model = GCNNUE05Net()
        export_quantized_binary_v5(model, out_nnue)

        test_fens = [
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "8/8/8/3k4/8/8/4B3/3K1N2 w - - 0 1",
            "8/8/8/3k4/8/8/3B4/3K1N2 b - - 12 7",
            "8/P1k5/8/1K6/8/8/8/8 b - - 4 40",
            "8/4k3/3p4/8/2B1P3/8/4b3/4K3 w - - 72 60",
        ]

        cmd_lines = [f"setoption name EvalFile value {out_nnue}", "setoption name UseNNUE value true"]
        for fen in test_fens:
            cmd_lines.append(f"position fen {fen}")
            cmd_lines.append("eval")
        cmd_lines.append("quit")

        proc = subprocess.run(
            [engine_path],
            input="\n".join(cmd_lines) + "\n",
            capture_output=True,
            text=True,
            check=True,
        )
        cpp_raw = []
        for line in proc.stdout.splitlines():
            if line.startswith("Raw NNUE Score"):
                cpp_raw.append(int(line.split(":")[1].strip().split()[0]))

        assert len(cpp_raw) == len(test_fens), f"Expected {len(test_fens)} eval lines, got {cpp_raw}\n{proc.stdout}"
        max_diff = 0
        for fen, c_val in zip(test_fens, cpp_raw):
            py_val = eval_v5_binary_int(out_nnue, fen)
            diff = abs(c_val - py_val)
            max_diff = max(max_diff, diff)
            print(f"  [PARITY] cpp={c_val:+6d} cp | py_int={py_val:+6d} cp | diff={diff} | fen={fen}")
        assert max_diff <= 1, f"Quantization/eval parity mismatch: max_diff={max_diff}"
        print(f"[+] GCNNUE05 C++ <-> Python parity PASSED (max_diff={max_diff} cp)")
    finally:
        if cleanup_temp and os.path.exists(out_nnue):
            try:
                os.remove(out_nnue)
            except OSError:
                pass


# ==============================================================================
# Kaggle / Interactive Notebook Configuration
# (Edit values here if running directly inside a Kaggle / Jupyter Notebook cell)
# ==============================================================================

def is_notebook():
    """Detects whether running inside a Jupyter, IPython, or Kaggle Notebook kernel."""
    try:
        from IPython import get_ipython
        if get_ipython() is not None:
            return True
    except Exception:
        pass
    return bool(os.environ.get("KAGGLE_KERNEL_RUN_TYPE") or "ipykernel" in sys.modules)


KAGGLE_CONFIG = {
    "arch": "v5",                  # "v5" (GCNNUE05 RelGeo + DrawGate) or "v4" (GCNNUE04 CP-HG)
    "data": None,                  # None = auto-detects all files in /kaggle/input
    "output": "/kaggle/working/gc.nnue" if os.path.isdir("/kaggle/working") else "gc.nnue",
    "epochs": 25,                  # Number of training epochs
    "batch_size": 8192,            # Batch size (8192 recommended for GPU P100 / T4 / L4)
    "lr": 0.001,                   # Learning rate
    "lambda_wdl": 0.65,            # Blends score and game result (WDL)
    "cp_loss_weight": 0.25,        # Non-saturating SmoothL1 centipawn loss weight
    "max_samples": None,           # None = full dataset; or integer for testing
    "val_split": 0.05,             # 5% for validation loss
    "device": "auto",              # "auto" (selects GPU if available), "cuda", or "cpu"
    "verify": True,                # Verify binary post-export
    "test_fen": None,
}


# ==============================================================================
# Training Pipeline
# ==============================================================================

def train_nnue(args=None, **kwargs):
    """
    Main training execution function.
    Can be called via CLI Namespace, with a dict, or with keyword arguments:
      train_nnue(data="/kaggle/input", epochs=25, batch_size=8192, device="cuda")
    """
    parser = build_parser()
    default_args, _ = parser.parse_known_args([])

    if args is None:
        cfg = dict(KAGGLE_CONFIG) if is_notebook() else {}
        cfg.update(kwargs)
        for k, v in cfg.items():
            if hasattr(default_args, k):
                setattr(default_args, k, v)
        args = default_args
    elif isinstance(args, dict):
        cfg = dict(args)
        cfg.update(kwargs)
        for k, v in cfg.items():
            if hasattr(default_args, k):
                setattr(default_args, k, v)
        args = default_args
    else:
        for k, v in kwargs.items():
            if hasattr(args, k):
                setattr(args, k, v)

    arch = getattr(args, "arch", "v5")
    is_v5 = (arch == "v5")

    # 1. Resolve Device
    if args.device == 'auto':
        device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    else:
        device = torch.device(args.device)
    print(f"Using device: {device} | Architecture: {arch.upper()} ({'GCNNUE05 RelGeo+DrawGate' if is_v5 else 'GCNNUE04 CP-HG'})")

    # 2. Load & Deduplicate Dataset (handles single/multi-file, directories, and auto-detect)
    teacher_path = getattr(args, "teacher", None)
    dataset = load_dataset(
        args.data,
        max_samples=args.max_samples,
        lambda_wdl=1.0 if teacher_path else args.lambda_wdl
    )
    total_samples = len(dataset)
    if total_samples == 0:
        raise ValueError("Dataset is empty! No valid training samples found.")

    if teacher_path:
        apply_teacher_distillation(dataset, teacher_path, args.lambda_wdl, device, qa=QA, qb=QB)

    # 3. Train / Validation Split (cap validation at 200,000 to save GPU compute and memory)
    val_size = min(int(total_samples * args.val_split), 200000) if args.val_split > 0 else 0
    train_size = total_samples - val_size
    print(f"Split: {train_size:,} train samples, {val_size:,} validation samples")

    perm = torch.randperm(total_samples, dtype=torch.int32)
    train_indices = perm[:train_size]
    val_indices = perm[train_size:]

    # 4. Initialize Model, Optimizer, Loss, and Scheduler
    model = GCNNUE05Net() if is_v5 else PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS)
    if getattr(args, "init_from", None):
        load_pretrained_binary(model, args.init_from, qa=QA, qb=QB)
    model = model.to(device)

    optimizer = optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4 if is_v5 else 1e-5)
    eta_ratio = 0.05 if is_v5 else 0.1
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs, eta_min=args.lr * eta_ratio)
    criterion = nn.BCEWithLogitsLoss()

    batch_size = args.batch_size
    cp_w = max(0.0, min(1.0, float(getattr(args, "cp_loss_weight", 0.25 if is_v5 else 0.0))))
    print(f"\nStarting training: {args.epochs} epochs | batch size: {batch_size} | lr: {args.lr} | lambda: {args.lambda_wdl} | cp_loss_weight: {cp_w}")
    print("-" * 75)

    def run_epoch(indices, is_train=True):
        model.train(is_train)
        n_samples = len(indices)
        if n_samples == 0:
            return 0.0, 0.0, 0.0

        epoch_perm = indices[torch.randperm(n_samples, dtype=torch.int32)] if is_train else indices
        total_loss = 0.0
        t0 = time.time()

        for start_idx in range(0, n_samples, batch_size):
            end_idx = min(start_idx + batch_size, n_samples)
            cur_b = end_idx - start_idx
            batch_slice = epoch_perm[start_idx:end_idx]

            bw = dataset.w_indices[batch_slice]
            bb = dataset.b_indices[batch_slice]
            valid_w = (bw >= 0)
            valid_b = (bb >= 0)
            feat_w = bw[valid_w].to(device=device, dtype=torch.long)
            feat_b = bb[valid_b].to(device=device, dtype=torch.long)
            off_w = torch.empty(cur_b, dtype=torch.long, device=device)
            off_b = torch.empty(cur_b, dtype=torch.long, device=device)
            off_w[0] = 0
            off_b[0] = 0
            if cur_b > 1:
                off_w[1:] = valid_w.sum(dim=1).cumsum(0)[:-1].to(device=device)
                off_b[1:] = valid_b.sum(dim=1).cumsum(0)[:-1].to(device=device)
            bstm = dataset.stm[batch_slice].to(device=device, dtype=torch.long)
            bbucket = dataset.buckets[batch_slice].to(device=device, dtype=torch.long)
            btarget = dataset.targets[batch_slice].to(device=device)

            if is_v5:
                bhmc = dataset.hmc[batch_slice].to(device=device, dtype=torch.long)
                bwdl = dataset.wdl[batch_slice].to(device=device)
                bcp = dataset.cp[batch_slice].to(device=device)
                frel, orel = compute_relgeo_packed_batch(bw, bstm, bhmc, device)

            with torch.set_grad_enabled(is_train):
                if is_v5:
                    logits, _, _, d_logit = model.forward_components(
                        feat_w, feat_b, bstm, frel, bbucket, off_w, off_b, orel
                    )
                    loss = criterion(logits, btarget)
                    if cp_w > 0.0:
                        target_logits = torch.clamp(bcp / SCALE, -4.0, 4.0) if not teacher_path else torch.logit(btarget.clamp(1e-4, 1.0 - 1e-4))
                        loss = (1.0 - cp_w) * loss + cp_w * F.smooth_l1_loss(logits, target_logits, beta=0.25)

                    clear_draw = ((bwdl == 0.5) & ((bcp.abs() <= 25.0) | (bhmc >= 48)))
                    clear_win = ((bwdl != 0.5) & (bcp.abs() >= 200.0) & (bhmc < 32))
                    if clear_draw.any():
                        loss = loss + 0.05 * F.binary_cross_entropy_with_logits(
                            d_logit[clear_draw], torch.full((int(clear_draw.sum()),), 0.85, device=device)
                        )
                    if clear_win.any():
                        loss = loss + 0.02 * F.binary_cross_entropy_with_logits(
                            d_logit[clear_win], torch.zeros((int(clear_win.sum()),), device=device)
                        )
                else:
                    logits = model(feat_w, feat_b, bstm, bbucket, off_w, off_b)
                    loss = criterion(logits, btarget)
                    if cp_w > 0.0:
                        target_logits = torch.logit(btarget.clamp(1e-4, 1.0 - 1e-4))
                        loss = (1.0 - cp_w) * loss + cp_w * F.smooth_l1_loss(logits, target_logits, beta=0.25)

                if is_train:
                    optimizer.zero_grad()
                    loss.backward()
                    optimizer.step()

            total_loss += loss.item() * cur_b

        elapsed = time.time() - t0
        avg_loss = total_loss / n_samples
        rate = n_samples / elapsed if elapsed > 0 else 0
        return avg_loss, rate, elapsed

    best_val_loss = float('inf')
    best_state = None

    for epoch in range(1, args.epochs + 1):
        train_loss, train_rate, train_time = run_epoch(train_indices, is_train=True)
        if val_size > 0:
            val_loss, _, _ = run_epoch(val_indices, is_train=False)
            val_str = f" | Val Loss: {val_loss:.5f}"
        else:
            val_loss = train_loss
            val_str = ""

        if val_loss < best_val_loss:
            best_val_loss = val_loss
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}

        lr_cur = scheduler.get_last_lr()[0]
        scheduler.step()

        print(f"Epoch {epoch:2d}/{args.epochs:2d} | Train Loss: {train_loss:.5f}{val_str} | "
              f"Rate: {train_rate:,.0f} pos/s ({train_time:.1f}s) | lr: {lr_cur:.6f}")

    if best_state is not None:
        model.load_state_dict({k: v.to(device) for k, v in best_state.items()})
        print(f"Restored best validation checkpoint (val_loss={best_val_loss:.5f}).")

    print("-" * 75)
    print("Training finished.")

    output_path = args.output
    export_quantized_binary(model, output_path, qa=QA, qb=QB)

    if args.verify:
        verify_exported_binary(output_path, test_fen=args.test_fen)

    return model


# ==============================================================================
# CLI Entrypoint
# ==============================================================================

def build_parser():
    parser = argparse.ArgumentParser(
        description="GC-Chess-Engine NNUE PyTorch Trainer and Binary Quantization Exporter.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "--arch",
        type=str,
        default="v5",
        choices=["v5", "v4"],
        help="Target NNUE architecture: v5 (GCNNUE05 RelGeo + DrawGate, 1.51 MB) or v4 (legacy GCNNUE04 CP-HG, 804 KB)."
    )
    parser.add_argument(
        "--data", "-d",
        nargs="*",
        default=None,
        help="Path(s) to training dataset file(s) or directory(ies). "
             "Supports multiple paths, directories (searched recursively, e.g. /kaggle/input), "
             "and glob patterns. Defaults to auto-detecting in /kaggle/input or local data/."
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default="/kaggle/working/gc.nnue" if os.path.isdir("/kaggle/working") else "gc.nnue",
        help="Path for exported binary NNUE weights file."
    )
    parser.add_argument(
        "--epochs", "-e",
        type=int,
        default=25,
        help="Number of training epochs."
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=8192,
        help="Training batch size."
    )
    parser.add_argument(
        "--lr",
        type=float,
        default=0.001,
        help="Learning rate for AdamW optimizer."
    )
    parser.add_argument(
        "--lambda-wdl",
        type=float,
        default=0.65,
        help="WDL target blending weight (0.0 = pure score, 1.0 = pure WDL outcome)."
    )
    parser.add_argument(
        "--max-samples", "-m",
        type=int,
        default=None,
        help="Maximum unique samples to load from dataset (None loads full file)."
    )
    parser.add_argument(
        "--val-split",
        type=float,
        default=0.05,
        help="Fraction of dataset to reserve for validation loss evaluation."
    )
    parser.add_argument(
        "--device",
        type=str,
        default="auto",
        choices=["auto", "cpu", "cuda"],
        help="Device to use for training ('auto', 'cpu', or 'cuda')."
    )
    parser.add_argument(
        "--no-verify",
        dest="verify",
        action="store_false",
        help="Disable automatic post-export binary verification."
    )
    parser.add_argument(
        "--test-fen",
        type=str,
        default=None,
        help="Optional test FEN string for post-export evaluation verification."
    )
    parser.add_argument(
        "--verify-only",
        type=str,
        default=None,
        help="Verify an existing .nnue file without running training."
    )
    parser.add_argument(
        "--init-from",
        type=str,
        default=None,
        help="Optional path to an existing .nnue file (GCNNUE05/04/03/02) to warm-start training."
    )
    parser.add_argument(
        "--teacher",
        type=str,
        default=None,
        help="Optional teacher .nnue file (GCNNUE05/04/03/02) for soft distillation blended with WDL."
    )
    parser.add_argument(
        "--cp-loss-weight",
        type=float,
        default=0.25,
        help="Blend weight [0..1] for non-saturating SmoothL1 centipawn-logit loss vs BCE."
    )
    parser.add_argument(
        "--test-parity",
        type=str,
        default=None,
        metavar="ENGINE_BIN",
        help="Path to engine binary (e.g. output/main.exe) to test Python<->C++ GCNNUE05 parity and exit."
    )
    parser.add_argument(
        "--export-dummy",
        type=str,
        default=None,
        metavar="PATH",
        help="Export an initialized dummy NNUE model to the given path and exit."
    )
    return parser


def main():
    parser = build_parser()
    args, unknown = parser.parse_known_args()

    if is_notebook():
        user_cli_flags = [a for a in sys.argv[1:] if not a.startswith("-f") and "kernel" not in a]
        if not user_cli_flags:
            print("[*] Kaggle/Jupyter Notebook environment detected.")
            print("    Applying KAGGLE_CONFIG interactive settings:")
            for k, v in KAGGLE_CONFIG.items():
                if hasattr(args, k):
                    setattr(args, k, v)
                    print(f"      - {k:14s}: {v}")
            print("=" * 75)

    if args.test_parity:
        out_path = args.output if "--output" in sys.argv or "-o" in sys.argv else None
        test_parity(args.test_parity, out_path)
        return

    if args.export_dummy:
        export_dummy_model(args.export_dummy, arch=args.arch, verify=args.verify, test_fen=args.test_fen)
        return

    if args.verify_only:
        verify_exported_binary(args.verify_only, test_fen=args.test_fen)
        return

    train_nnue(args)


if __name__ == "__main__":
    main()
