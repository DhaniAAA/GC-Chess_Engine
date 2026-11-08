#!/usr/bin/env python3
"""
train_nnue.py — PyTorch NNUE Trainer & Quantization Binary Exporter for GC-Chess-Engine.

Architecture:
  Perspective (768 -> 512)x2 -> 1 with Squared Clipped ReLU (SCReLU) & 8 Output Buckets
  Feature Transformer:
    - 768 binary input features per perspective (White POV and Black POV)
    - FeatureIdx = color_rel * 384 + (pt - 1) * 64 + sq_rel
    - Accumulator: 512 neurons with int16 weights & biases
  Activation:
    - SCReLU: clamp(x, 0.0, 1.0)^2 in float space (clamp(x, 0, QA)^2 in int16 space, QA=255)
  Output Layer (8 Output Buckets based on piece count):
    - Side-to-Move (STM) concatenation: [y_stm (512), y_nstm (512)] -> 1024 neurons
    - 8 Buckets: bucket = clamp((piece_count - 1) // 4, 0, 7)
    - int16[8][1024] output weights (16,384 bytes) and int32[8] output biases (32 bytes)
  Loss:
    - BCEWithLogitsLoss with blended target: (1 - lambda) * sigmoid(score/400) + lambda * wdl
  Binary Format (gc.nnue):
    - Magic: "GCNNUE03" (8 bytes)
    - Feature weights: int16[768][512] (786,432 bytes, feature-major)
    - Feature biases: int16[512] (1,024 bytes)
    - Output weights: int16[8][1024] (16,384 bytes: 8 output buckets)
    - Output biases:  int32[8] (32 bytes, scaled by QA * QB = 16320)
    - Total exact file size: 803,880 bytes (Backward compatible with GCNNUE02 789,516 bytes)
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
import torch.optim as optim

# ==============================================================================
# Constants & Specifications
# ==============================================================================

MAGIC_HEADER_V2 = b"GCNNUE02"
MAGIC_HEADER_V3 = b"GCNNUE03"
MAGIC_HEADER = MAGIC_HEADER_V3
QA = 255
QB = 64
SCALE = 400
EXPECTED_FILE_SIZE_V2 = 789516  # 8 + 786432 + 1024 + 2048 + 4 bytes
EXPECTED_FILE_SIZE_V3 = 803880  # 8 + 786432 + 1024 + 16384 + 32 bytes
EXPECTED_FILE_SIZE = EXPECTED_FILE_SIZE_V3
NUM_FEATURES = 768
HIDDEN_SIZE = 512
OUTPUT_INPUT_SIZE = HIDDEN_SIZE * 2  # 1024
OUTPUT_BUCKETS = 8


def get_output_bucket(num_pieces):
    """
    Computes output bucket index [0..7] based on total piece count:
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

    Feature Indexing Formula:
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

            # White POV (persp = 0):
            # color_rel = c ^ 0 = c
            # sq_rel = sq
            w_idx = c * 384 + (pt - 1) * 64 + sq
            w_indices.append(w_idx)

            # Black POV (persp = 1):
            # color_rel = c ^ 1
            # sq_rel = sq ^ 56 (vertical flip)
            b_idx = (c ^ 1) * 384 + (pt - 1) * 64 + (sq ^ 56)
            b_indices.append(b_idx)

            file += 1

    return w_indices, b_indices, stm


def parse_data_line(line, lambda_wdl=0.5):
    """
    Parses a line from the dataset formatted as 'FEN | score | wdl' (or pure FEN).

    Score in dataset is White POV (centipawns).
    WDL in dataset is White POV (1.0 = Win, 0.5 = Draw, 0.0 = Loss).

    Returns:
      (w_indices, b_indices, stm, target, norm_fen, bucket) or None if malformed.
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

    # Convert score and wdl to side-to-move perspective
    score_stm = score if stm == 0 else -score
    wdl_stm = wdl if stm == 0 else (1.0 - wdl)

    # Sigmoid winning probability from score: P = sigmoid(score_stm / 400.0)
    scaled_score = max(-15.0, min(15.0, score_stm / 400.0))
    p_score = 1.0 / (1.0 + math.exp(-scaled_score))

    # Blended loss target: (1 - lambda) * P_score + lambda * WDL_stm
    target = (1.0 - lambda_wdl) * p_score + lambda_wdl * wdl_stm

    # Normalized FEN for deduplication (board, stm, castling, ep square)
    norm_fen = " ".join(tokens[:4])

    w_indices, b_indices, _ = parse_fen_to_features(fen)
    bucket = get_output_bucket(len(w_indices))
    return w_indices, b_indices, stm, target, norm_fen, bucket


# ==============================================================================
# Fast In-Memory Dataset
# ==============================================================================

class ChessNNUEDataset:
    """
    Compact, cache-friendly in-memory dataset storing active feature indices.
    Each chess position has at most 32 pieces, enabling fast vectorized batch collation.
    """
    def __init__(self, w_indices, b_indices, stm, targets, buckets):
        self.w_indices = w_indices  # torch.int16 tensor of shape (N, 32)
        self.b_indices = b_indices  # torch.int16 tensor of shape (N, 32)
        self.stm = stm              # torch.uint8 tensor of shape (N,)
        self.targets = targets      # torch.float32 tensor of shape (N,)
        self.buckets = buckets      # torch.uint8 tensor of shape (N,)
        self.length = len(stm)

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
    float tgt[BLOCK_CAP];
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

        // Normalized FEN for deduplication (board + stm + castling + ep)
        char* norm_end = ptr;
        int spaces = 0;
        while (*norm_end && *norm_end != '|' && *norm_end != '\n' && *norm_end != '\r') {
            if (*norm_end == ' ') {
                spaces++;
                if (spaces >= 3) break;
            }
            norm_end++;
        }

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
        int b_idx = (nw - 1) / 4;
        if (b_idx < 0) b_idx = 0;
        if (b_idx > 7) b_idx = 7;
        cur_block->bucket[idx] = (uint8_t)b_idx;
        cur_block->tgt[idx] = target;
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

void fast_loader_copy_to_numpy(int16_t* out_w, int16_t* out_b, uint8_t* out_stm, uint8_t* out_bucket, float* out_tgt) {
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
                memcpy(out_tgt + offset, b->tgt, (size_t)cnt * sizeof(float));
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
            ctypes.POINTER(ctypes.c_float)
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
        tgt_arr = np.empty(count, dtype=np.float32)

        fast_loader.fast_loader_copy_to_numpy(
            w_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
            b_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
            stm_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            bucket_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            tgt_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        )
        fast_loader.fast_loader_cleanup()

        elapsed = time.time() - start_time
        dup_count = total_lines - count
        overall_rate = total_lines / elapsed if elapsed > 0 else 0
        mem_mb = (w_arr.nbytes + b_arr.nbytes + stm_arr.nbytes + bucket_arr.nbytes + tgt_arr.nbytes) / (1024 * 1024)
        print(f"Dataset loaded: {count:,} unique positions from {total_lines:,} lines "
              f"({dup_count:,} duplicates filtered) in {elapsed:.2f}s ({overall_rate:,.0f} lines/s)")
        print(f"[*] Memory Footprint: {mem_mb:.1f} MB (compact int16 layout, zero memory duplication)")

        # Convert to PyTorch tensors with ZERO memory copy (shares numpy buffer directly)
        w_tensor = torch.from_numpy(w_arr)
        b_tensor = torch.from_numpy(b_arr)
        stm_tensor = torch.from_numpy(stm_arr)
        tgt_tensor = torch.from_numpy(tgt_arr)
        bucket_tensor = torch.from_numpy(bucket_arr)

        import gc
        gc.collect()
        return ChessNNUEDataset(w_tensor, b_tensor, stm_tensor, tgt_tensor, bucket_tensor)

    # --- Mode 2: Optimized Python Chunked Loader (Fallback) ---
    print("[*] Engine Mode: Python Chunked Dataset Loader (Fallback)")
    chunk_size = 100000
    w_chunks = []
    b_chunks = []
    stm_chunks = []
    bucket_chunks = []
    tgt_chunks = []

    cur_w = np.full((chunk_size, 32), -1, dtype=np.int16)
    cur_b = np.full((chunk_size, 32), -1, dtype=np.int16)
    cur_stm = np.zeros(chunk_size, dtype=np.uint8)
    cur_bucket = np.zeros(chunk_size, dtype=np.uint8)
    cur_tgt = np.zeros(chunk_size, dtype=np.float32)
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

                w_idx, b_idx, stm, target, norm_fen, bucket = parsed

                # 64-bit integer hash for fast deduplication
                h = zlib.crc32(norm_fen.encode('ascii'))
                if h in seen_hashes:
                    continue
                seen_hashes.add(h)

                if cur_count >= chunk_size:
                    w_chunks.append(cur_w)
                    b_chunks.append(cur_b)
                    stm_chunks.append(cur_stm)
                    bucket_chunks.append(cur_bucket)
                    tgt_chunks.append(cur_tgt)
                    cur_w = np.full((chunk_size, 32), -1, dtype=np.int16)
                    cur_b = np.full((chunk_size, 32), -1, dtype=np.int16)
                    cur_stm = np.zeros(chunk_size, dtype=np.uint8)
                    cur_bucket = np.zeros(chunk_size, dtype=np.uint8)
                    cur_tgt = np.zeros(chunk_size, dtype=np.float32)
                    cur_count = 0

                nw = min(len(w_idx), 32)
                nb = min(len(b_idx), 32)
                cur_w[cur_count, :nw] = w_idx[:nw]
                cur_b[cur_count, :nb] = b_idx[:nb]
                cur_stm[cur_count] = stm
                cur_bucket[cur_count] = bucket
                cur_tgt[cur_count] = target
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
        tgt_chunks.append(cur_tgt[:cur_count])

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
    tgt_arr = np.concatenate(tgt_chunks, axis=0)
    del w_chunks, b_chunks, stm_chunks, bucket_chunks, tgt_chunks

    w_tensor = torch.from_numpy(w_arr)
    b_tensor = torch.from_numpy(b_arr)
    stm_tensor = torch.from_numpy(stm_arr)
    tgt_tensor = torch.from_numpy(tgt_arr)
    bucket_tensor = torch.from_numpy(bucket_arr)

    import gc
    gc.collect()
    return ChessNNUEDataset(w_tensor, b_tensor, stm_tensor, tgt_tensor, bucket_tensor)


# ==============================================================================
# NNUE PyTorch Architecture
# ==============================================================================

class PerspectiveNNUE(nn.Module):
    """
    Perspective (768 -> 512)x2 -> 1 SCReLU Network with 8 Output Buckets.

    Features:
      - Feature transformer (768, 512) with bias.
      - Dual accumulators for White POV and Black POV.
      - SCReLU activation: clamp(x, 0.0, 1.0) ** 2.
      - Side-to-move permutation: [STM_act (512), NSTM_act (512)] -> (1024).
      - Output layer: 8 buckets of Linear(1024 -> 1) weights and biases.
      - Logits represent normalized winning potential (score / 400.0).
    """
    def __init__(self, num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS):
        super().__init__()
        self.num_features = num_features
        self.hidden_size = hidden_size
        self.output_buckets = output_buckets

        # Linear projection 768 -> 512
        self.ft = nn.Linear(num_features, hidden_size)

        # Output projection: 8 output buckets [8, 1024] and biases [8]
        self.out_weights = nn.Parameter(torch.empty(output_buckets, hidden_size * 2))
        self.out_biases = nn.Parameter(torch.empty(output_buckets))

        # Initialize weights carefully for SCReLU and QA=255 scaling
        self._init_weights()

    def _init_weights(self):
        # Feature transformer: small std so sum over ~30 pieces remains within [0, 1]
        nn.init.normal_(self.ft.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.ft.bias, 0.1)

        # Output buckets: small std for stable initial gradients
        nn.init.normal_(self.out_weights, mean=0.0, std=0.02)
        nn.init.constant_(self.out_biases, 0.0)

    def forward(self, feat_w, feat_b, stm, buckets=None):
        """
        Forward pass.
        feat_w: (B, 768) binary float tensor for White POV features
        feat_b: (B, 768) binary float tensor for Black POV features
        stm: (B,) long tensor (0 for White, 1 for Black)
        buckets: (B,) long/int tensor of bucket indices [0..7]
        """
        # Feature transformer projections
        acc_w = self.ft(feat_w)
        acc_b = self.ft(feat_b)

        # SCReLU activation in float normalized space [0.0, 1.0]
        act_w = torch.clamp(acc_w, 0.0, 1.0) ** 2
        act_b = torch.clamp(acc_b, 0.0, 1.0) ** 2

        # Side to move alignment:
        # If stm == 0 (White to move): STM is White, NSTM is Black
        # If stm == 1 (Black to move): STM is Black, NSTM is White
        stm_mask = (stm.unsqueeze(-1) == 0)
        stm_act = torch.where(stm_mask, act_w, act_b)
        nstm_act = torch.where(stm_mask, act_b, act_w)

        # Concatenate 512 + 512 = 1024
        combined = torch.cat([stm_act, nstm_act], dim=-1)

        if buckets is None:
            # Fallback to bucket 7 if not specified
            w = self.out_weights[7].unsqueeze(0)
            b = self.out_biases[7]
            logits = (combined * w).sum(dim=-1) + b
        else:
            w = self.out_weights[buckets.long()]  # Shape: (B, 1024)
            b = self.out_biases[buckets.long()]   # Shape: (B,)
            logits = (combined * w).sum(dim=-1) + b

        return logits

    def evaluate_cp(self, feat_w, feat_b, stm, buckets=None):
        """Evaluates position in centipawns."""
        with torch.no_grad():
            logits = self.forward(feat_w, feat_b, stm, buckets)
            return logits * SCALE


# ==============================================================================
# Model Quantization & Binary Exporter
# ==============================================================================

def export_quantized_binary(model, output_path, qa=QA, qb=QB):
    """
    Quantizes floating-point weights to integers and exports to gc.nnue (v3 8-bucket format):
      1. Magic Header: 'GCNNUE03' (8 bytes)
      2. Feature Weights: int16[768][512] (786,432 bytes, feature-major)
      3. Feature Biases: int16[512] (1,024 bytes)
      4. Output Weights: int16[8][1024] (16,384 bytes: 8 output buckets)
      5. Output Biases: int32[8] (32 bytes, scaled by QA * QB = 16320)
    Total exact size: 803,880 bytes.
    """
    model.eval()
    with torch.no_grad():
        # Feature weights: Linear(768, 512) has shape (512, 768).
        # Transpose to (768, 512) for feature-major layout:
        # row f contains the 512 weights for feature f.
        w_ft = model.ft.weight.data.t().cpu().float()
        w_ft_q = torch.clamp(torch.round(w_ft * qa), -32768, 32767).to(torch.int16)

        # Feature biases: shape (512,)
        b_ft = model.ft.bias.data.cpu().float()
        b_ft_q = torch.clamp(torch.round(b_ft * qa), -32768, 32767).to(torch.int16)

        # Output weights: shape (8, 1024)
        w_out = model.out_weights.data.cpu().float()
        w_out_q = torch.clamp(torch.round(w_out * qb), -32768, 32767).to(torch.int16)

        # Output biases: shape (8,) -> scaled by QA * QB = 16320
        b_out = model.out_biases.data.cpu().float()
        scale_bias = qa * qb
        b_out_q = torch.round(b_out * scale_bias).to(torch.int32)

        out_file = Path(output_path)
        out_file.parent.mkdir(parents=True, exist_ok=True)

        with open(out_file, "wb") as f:
            f.write(MAGIC_HEADER_V3)
            f.write(w_ft_q.numpy().tobytes())
            f.write(b_ft_q.numpy().tobytes())
            f.write(w_out_q.numpy().tobytes())
            f.write(b_out_q.numpy().tobytes())

        actual_size = os.path.getsize(output_path)
        if actual_size != EXPECTED_FILE_SIZE_V3:
            raise ValueError(
                f"Export size error: got {actual_size} bytes, expected exactly {EXPECTED_FILE_SIZE_V3} bytes!"
            )

        print(f"\n[+] Successfully exported quantized NNUE model:")
        print(f"    File: {out_file.resolve()}")
        print(f"    Magic: {MAGIC_HEADER_V3.decode('ascii')}")
        print(f"    Size: {actual_size:,} bytes (verified exact)")
        print(f"    Feature Weights: int16[{NUM_FEATURES}][{HIDDEN_SIZE}] ({w_ft_q.numel() * 2:,} bytes)")
        print(f"    Feature Biases:  int16[{HIDDEN_SIZE}] ({b_ft_q.numel() * 2:,} bytes)")
        print(f"    Output Weights:  int16[{OUTPUT_BUCKETS}][{OUTPUT_INPUT_SIZE}] ({w_out_q.numel() * 2:,} bytes)")
        print(f"    Output Biases:   int32[{OUTPUT_BUCKETS}] ({b_out_q.numel() * 4:,} bytes, scaled by {scale_bias})")


def verify_exported_binary(filepath, test_fen=None):
    """
    Verifies the binary file format, integrity, and computes a test evaluation
    using exact integer arithmetic matching the C++ engine implementation.
    Supports both GCNNUE03 (8 output buckets) and legacy GCNNUE02 (single bucket).
    """
    print(f"\n[*] Verifying binary file: {filepath}")
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"Binary file not found: {filepath}")

    size = os.path.getsize(filepath)
    if size != EXPECTED_FILE_SIZE_V3 and size != EXPECTED_FILE_SIZE_V2:
        raise ValueError(f"Size mismatch: {size} != {EXPECTED_FILE_SIZE_V3} (v3) or {EXPECTED_FILE_SIZE_V2} (v2)")

    with open(filepath, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC_HEADER_V3 and magic != MAGIC_HEADER_V2:
            raise ValueError(f"Magic header mismatch: {magic}")

        w_ft_bytes = f.read(NUM_FEATURES * HIDDEN_SIZE * 2)
        b_ft_bytes = f.read(HIDDEN_SIZE * 2)

        if magic == MAGIC_HEADER_V3:
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

    # Verify no NaN or corrupt values
    assert not np.isnan(w_ft).any(), "NaN found in feature weights"
    assert not np.isnan(b_ft).any(), "NaN found in feature biases"
    assert not np.isnan(w_out).any(), "NaN found in output weights"

    print(f"    Magic: {magic.decode('ascii')} [OK]")
    print(f"    Feature Weights: min={w_ft.min()}, max={w_ft.max()}, mean={w_ft.mean():.2f}")
    print(f"    Feature Biases:  min={b_ft.min()}, max={b_ft.max()}, mean={b_ft.mean():.2f}")
    print(f"    Output Weights:  min={w_out.min()}, max={w_out.max()}, mean={w_out.mean():.2f}")
    print(f"    Output Biases:   min={b_out.min()}, max={b_out.max()}, mean={b_out.mean():.2f}")

    # Evaluate test position
    if test_fen is None:
        test_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

    w_idx, b_idx, stm = parse_fen_to_features(test_fen)
    bucket = get_output_bucket(len(w_idx))

    # Integer accumulator refresh
    acc_w = b_ft.astype(np.int32) + w_ft[w_idx].astype(np.int32).sum(axis=0)
    acc_b = b_ft.astype(np.int32) + w_ft[b_idx].astype(np.int32).sum(axis=0)

    # SCReLU: clamp(x, 0, QA)^2
    y_w = np.clip(acc_w, 0, QA) ** 2
    y_b = np.clip(acc_b, 0, QA) ** 2

    y_stm = y_w if stm == 0 else y_b
    y_nstm = y_b if stm == 0 else y_w

    # Output projection in integer space using active bucket
    bucket_w = w_out[bucket]
    bucket_b = b_out[bucket]
    raw = int(np.dot(y_stm, bucket_w[:HIDDEN_SIZE].astype(np.int64)) +
              np.dot(y_nstm, bucket_w[HIDDEN_SIZE:].astype(np.int64)))

    def c_div(a, b):
        return int(a / b)

    term = c_div(raw, QA) + int(bucket_b)
    eval_cp = c_div(term * SCALE, QA * QB)

    print(f"    Test FEN Evaluation ({'White' if stm == 0 else 'Black'} to move, Bucket {bucket}):")
    print(f"      FEN:  {test_fen}")
    print(f"      Raw:  {raw}")
    print(f"      Eval: {eval_cp} cp")
    print("    Binary verification passed successfully!\n")
    return eval_cp


def export_dummy_model(output_path, verify=True, test_fen=None, seed=42):
    """
    Creates an initialized dummy 512-neuron NNUE model with 8 output buckets and exports it.
    Can be used for immediate testing and C++ engine verification.
    """
    print(f"\n[*] Generating dummy 512-neuron NNUE model with 8 output buckets (seed={seed})...")
    torch.manual_seed(seed)
    np.random.seed(seed)
    model = PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS)
    export_quantized_binary(model, output_path)
    if verify:
        verify_exported_binary(output_path, test_fen=test_fen)
    export_quantized_binary(model, output_path)
    if verify:
        verify_exported_binary(output_path, test_fen=test_fen)


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
    "data": None,                  # None = auto-detects all files in /kaggle/input
    "output": "/kaggle/working/gc.nnue" if os.path.isdir("/kaggle/working") else "gc.nnue",
    "epochs": 30,                  # Number of training epochs
    "batch_size": 16384,           # Batch size (16384 recommended for GPU P100 / T4)
    "lr": 0.001,                   # Learning rate
    "lambda_wdl": 0.5,             # 0.5 blends score and game result (WDL)
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
      train_nnue(data="/kaggle/input", epochs=20, batch_size=16384, device="cuda")
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

    # 1. Resolve Device
    if args.device == 'auto':
        device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # 2. Load & Deduplicate Dataset (handles single/multi-file, directories, and auto-detect)
    dataset = load_dataset(
        args.data,
        max_samples=args.max_samples,
        lambda_wdl=args.lambda_wdl
    )
    total_samples = len(dataset)
    if total_samples == 0:
        raise ValueError("Dataset is empty! No valid training samples found.")

    # 4. Train / Validation Split (cap validation at 200,000 to save GPU compute and memory)
    val_size = min(int(total_samples * args.val_split), 200000) if args.val_split > 0 else 0
    train_size = total_samples - val_size
    print(f"Split: {train_size:,} train samples, {val_size:,} validation samples")

    # Permute dataset (using int32 to minimize memory usage)
    perm = torch.randperm(total_samples, dtype=torch.int32)
    train_indices = perm[:train_size]
    val_indices = perm[train_size:]

    # 5. Initialize Model, Optimizer, Loss, and Scheduler
    model = PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE, output_buckets=OUTPUT_BUCKETS).to(device)
    optimizer = optim.Adam(model.parameters(), lr=args.lr, weight_decay=1e-5)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs, eta_min=args.lr * 0.1)
    criterion = nn.BCEWithLogitsLoss()

    batch_size = args.batch_size
    print(f"\nStarting training: {args.epochs} epochs | batch size: {batch_size} | lr: {args.lr} | lambda: {args.lambda_wdl}")
    print("-" * 75)

    def run_epoch(epoch, indices, is_train=True):
        if is_train:
            model.train()
        else:
            model.eval()

        n_samples = len(indices)
        if n_samples == 0:
            return 0.0

        if is_train:
            # Shuffle training indices each epoch (using int32)
            epoch_perm = indices[torch.randperm(n_samples, dtype=torch.int32)]
        else:
            epoch_perm = indices

        total_loss = 0.0
        n_batches = 0
        t0 = time.time()

        for start_idx in range(0, n_samples, batch_size):
            end_idx = min(start_idx + batch_size, n_samples)
            cur_b = end_idx - start_idx
            batch_slice = epoch_perm[start_idx:end_idx]

            # Vectorized feature tensor construction: transfer and cast only mini-batch to GPU
            bw = dataset.w_indices[batch_slice]
            bb = dataset.b_indices[batch_slice]
            bstm = dataset.stm[batch_slice].to(device=device, dtype=torch.long)
            bbucket = dataset.buckets[batch_slice].to(device=device, dtype=torch.long)
            btarget = dataset.targets[batch_slice].to(device=device)

            feat_w = torch.zeros(cur_b, NUM_FEATURES, device=device)
            feat_b = torch.zeros(cur_b, NUM_FEATURES, device=device)

            valid_w = (bw >= 0)
            feat_w.scatter_add_(1, bw.to(device=device, dtype=torch.int64).clamp(min=0), valid_w.to(device=device, dtype=torch.float32))

            valid_b = (bb >= 0)
            feat_b.scatter_add_(1, bb.to(device=device, dtype=torch.int64).clamp(min=0), valid_b.to(device=device, dtype=torch.float32))

            if is_train:
                optimizer.zero_grad()
                logits = model(feat_w, feat_b, bstm, bbucket)
                loss = criterion(logits, btarget)
                loss.backward()
                optimizer.step()
            else:
                with torch.no_grad():
                    logits = model(feat_w, feat_b, bstm, bbucket)
                    loss = criterion(logits, btarget)

            total_loss += loss.item() * cur_b
            n_batches += 1

        elapsed = time.time() - t0
        avg_loss = total_loss / n_samples
        rate = n_samples / elapsed if elapsed > 0 else 0
        return avg_loss, rate, elapsed

    best_val_loss = float('inf')

    for epoch in range(1, args.epochs + 1):
        train_loss, train_rate, train_time = run_epoch(epoch, train_indices, is_train=True)
        if val_size > 0:
            val_loss, _, _ = run_epoch(epoch, val_indices, is_train=False)
            val_str = f" | Val Loss: {val_loss:.5f}"
        else:
            val_loss = train_loss
            val_str = ""

        lr_cur = scheduler.get_last_lr()[0]
        scheduler.step()

        print(f"Epoch {epoch:2d}/{args.epochs:2d} | Train Loss: {train_loss:.5f}{val_str} | "
              f"Rate: {train_rate:,.0f} pos/s ({train_time:.1f}s) | lr: {lr_cur:.6f}")

    print("-" * 75)
    print("Training finished.")

    # 6. Quantize & Export
    output_path = args.output
    export_quantized_binary(model, output_path, qa=QA, qb=QB)

    # 7. Verification
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
        default=10,
        help="Number of training epochs."
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=2048,
        help="Training batch size."
    )
    parser.add_argument(
        "--lr",
        type=float,
        default=0.001,
        help="Learning rate for Adam optimizer."
    )
    parser.add_argument(
        "--lambda-wdl",
        type=float,
        default=0.5,
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
        "--export-dummy",
        type=str,
        default=None,
        metavar="PATH",
        help="Export an initialized dummy 512-neuron NNUE model to the given path and exit."
    )
    return parser


def main():
    parser = build_parser()
    # parse_known_args() safely filters Jupyter/Kaggle notebook kernel flags (like -f /root/.../kernel.json)
    args, unknown = parser.parse_known_args()

    # If running in a Jupyter/Kaggle notebook without custom CLI flags, adopt KAGGLE_CONFIG
    if is_notebook():
        user_cli_flags = [a for a in sys.argv[1:] if not a.startswith("-f") and "kernel" not in a]
        if not user_cli_flags:
            print("[*] Kaggle/Jupyter Notebook environment detected.")
            print("    Applying KAGGLE_CONFIG interactive settings:")
            for k, v in KAGGLE_CONFIG.items():
                if hasattr(args, k):
                    setattr(args, k, v)
                    print(f"      - {k:12s}: {v}")
            print("=" * 75)

    if args.export_dummy:
        export_dummy_model(args.export_dummy, verify=args.verify, test_fen=args.test_fen)
        return

    if args.verify_only:
        verify_exported_binary(args.verify_only, test_fen=args.test_fen)
        return

    train_nnue(args)


if __name__ == "__main__":
    main()
