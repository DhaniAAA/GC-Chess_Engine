#!/usr/bin/env python3
"""
train_nnue.py — PyTorch NNUE Trainer & Quantization Binary Exporter for GC-Chess-Engine.

Architecture:
  Perspective (768 -> 512)x2 -> 1 with Squared Clipped ReLU (SCReLU)
  Feature Transformer:
    - 768 binary input features per perspective (White POV and Black POV)
    - FeatureIdx = color_rel * 384 + (pt - 1) * 64 + sq_rel
    - Accumulator: 512 neurons with int16 weights & biases
  Activation:
    - SCReLU: clamp(x, 0.0, 1.0)^2 in float space (clamp(x, 0, QA)^2 in int16 space, QA=255)
  Output Layer:
    - Side-to-Move (STM) concatenation: [y_stm (512), y_nstm (512)] -> 1024 neurons
    - Linear(1024 -> 1) with int16 weights and int32 bias
  Loss:
    - BCEWithLogitsLoss with blended target: (1 - lambda) * sigmoid(score/400) + lambda * wdl
  Binary Format (gc.nnue):
    - Magic: "GCNNUE02" (8 bytes)
    - Feature weights: int16[768][512] (786,432 bytes, feature-major)
    - Feature biases: int16[512] (1,024 bytes)
    - Output weights: int16[1024] (2,048 bytes: 0..511 STM, 512..1023 NSTM)
    - Output bias: int32[1] (4 bytes, scaled by QA * QB = 16320)
    - Total exact file size: 789,516 bytes
"""

import argparse
import math
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

# ==============================================================================
# Constants & Specifications
# ==============================================================================

MAGIC_HEADER = b"GCNNUE02"
QA = 255
QB = 64
SCALE = 400
EXPECTED_FILE_SIZE = 789516  # 8 + 786432 + 1024 + 2048 + 4 bytes
NUM_FEATURES = 768
HIDDEN_SIZE = 512
OUTPUT_INPUT_SIZE = HIDDEN_SIZE * 2  # 1024

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
      (w_indices, b_indices, stm, target, norm_fen) or None if malformed.
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
    return w_indices, b_indices, stm, target, norm_fen


# ==============================================================================
# Fast In-Memory Dataset
# ==============================================================================

class ChessNNUEDataset:
    """
    Compact, cache-friendly in-memory dataset storing active feature indices.
    Each chess position has at most 32 pieces, enabling fast vectorized batch collation.
    """
    def __init__(self, w_indices, b_indices, stm, targets):
        self.w_indices = w_indices  # torch.int64 tensor of shape (N, 32)
        self.b_indices = b_indices  # torch.int64 tensor of shape (N, 32)
        self.stm = stm              # torch.long tensor of shape (N,)
        self.targets = targets      # torch.float32 tensor of shape (N,)
        self.length = len(stm)

    def __len__(self):
        return self.length


def load_dataset(data_path, max_samples=None, lambda_wdl=0.5, log_interval=100000):
    """
    Reads dataset file(s) with FEN deduplication and converts to tensor arrays.
    Supports either a single file (e.g. dataset_all.txt) or a folder containing txt chunks.
    """
    path = Path(data_path)
    files_to_read = []
    if path.is_file():
        files_to_read.append(path)
    elif path.is_dir():
        if (path / "dataset_all.txt").is_file():
            files_to_read.append(path / "dataset_all.txt")
        else:
            chunks = sorted(path.glob("chunk_*.txt"))
            if chunks:
                files_to_read.extend(chunks)
            else:
                txts = sorted(path.glob("*.txt"))
                if txts:
                    files_to_read.extend(txts)
    if not files_to_read:
        raise FileNotFoundError(f"No valid dataset file(s) found at: {data_path}")

    total_bytes = sum(f.stat().st_size for f in files_to_read)
    print(f"Loading dataset from {len(files_to_read)} file(s) ({total_bytes / (1024*1024):.1f} MB total)...")

    # Initial capacity
    cap = max_samples if max_samples is not None and max_samples > 0 else 500000
    w_arr = np.full((cap, 32), -1, dtype=np.int16)
    b_arr = np.full((cap, 32), -1, dtype=np.int16)
    stm_arr = np.zeros(cap, dtype=np.uint8)
    tgt_arr = np.zeros(cap, dtype=np.float32)

    seen_fens = set()
    count = 0
    total_lines = 0
    start_time = time.time()

    for fpath in files_to_read:
        with open(fpath, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                total_lines += 1
                parsed = parse_data_line(line, lambda_wdl=lambda_wdl)
                if parsed is None:
                    continue

                w_idx, b_idx, stm, target, norm_fen = parsed

                # Deduplication check
                if norm_fen in seen_fens:
                    continue
                seen_fens.add(norm_fen)

                # Resize if necessary
                if count >= cap:
                    if max_samples is not None and count >= max_samples:
                        break
                    new_cap = int(cap * 1.5)
                    w_arr = np.pad(w_arr, ((0, new_cap - cap), (0, 0)), constant_values=-1)
                    b_arr = np.pad(b_arr, ((0, new_cap - cap), (0, 0)), constant_values=-1)
                    stm_arr = np.pad(stm_arr, (0, new_cap - cap), constant_values=0)
                    tgt_arr = np.pad(tgt_arr, (0, new_cap - cap), constant_values=0.0)
                    cap = new_cap

                nw = min(len(w_idx), 32)
                nb = min(len(b_idx), 32)
                w_arr[count, :nw] = w_idx[:nw]
                b_arr[count, :nb] = b_idx[:nb]
                stm_arr[count] = stm
                tgt_arr[count] = target
                count += 1

                if max_samples is not None and count >= max_samples:
                    break

                if total_lines % log_interval == 0:
                    elapsed = time.time() - start_time
                    rate = total_lines / elapsed if elapsed > 0 else 0
                    print(f"  Processed {total_lines:,} lines | Unique kept: {count:,} ({rate:.0f} lines/s)")

    elapsed = time.time() - start_time
    dup_count = total_lines - count
    print(f"Dataset loaded: {count:,} unique positions from {total_lines:,} lines "
          f"({dup_count:,} duplicates filtered) in {elapsed:.2f}s")

    # Slice to exact count and convert to PyTorch tensors
    w_tensor = torch.from_numpy(w_arr[:count].astype(np.int64))
    b_tensor = torch.from_numpy(b_arr[:count].astype(np.int64))
    stm_tensor = torch.from_numpy(stm_arr[:count].astype(np.int64))
    tgt_tensor = torch.from_numpy(tgt_arr[:count])

    return ChessNNUEDataset(w_tensor, b_tensor, stm_tensor, tgt_tensor)


# ==============================================================================
# NNUE PyTorch Architecture
# ==============================================================================

class PerspectiveNNUE(nn.Module):
    """
    Perspective (768 -> 512)x2 -> 1 SCReLU Network.

    Features:
      - Feature transformer (768, 512) with bias.
      - Dual accumulators for White POV and Black POV.
      - SCReLU activation: clamp(x, 0.0, 1.0) ** 2.
      - Side-to-move permutation: [STM_act (512), NSTM_act (512)] -> (1024).
      - Output layer: Linear(1024, 1) with bias.
      - Logits represent normalized winning potential (score / 400.0).
    """
    def __init__(self, num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE):
        super().__init__()
        self.num_features = num_features
        self.hidden_size = hidden_size

        # Linear projection 768 -> 512
        self.ft = nn.Linear(num_features, hidden_size)

        # Output projection 1024 -> 1
        self.out = nn.Linear(hidden_size * 2, 1)

        # Initialize weights carefully for SCReLU and QA=255 scaling
        self._init_weights()

    def _init_weights(self):
        # Feature transformer: small std so sum over ~30 pieces remains within [0, 1]
        nn.init.normal_(self.ft.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.ft.bias, 0.1)

        # Output layer: small std for stable initial gradients
        nn.init.normal_(self.out.weight, mean=0.0, std=0.02)
        nn.init.constant_(self.out.bias, 0.0)

    def forward(self, feat_w, feat_b, stm):
        """
        Forward pass.
        feat_w: (B, 768) binary float tensor for White POV features
        feat_b: (B, 768) binary float tensor for Black POV features
        stm: (B,) long tensor (0 for White, 1 for Black)
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

        # Output logits
        logits = self.out(combined).squeeze(-1)
        return logits

    def evaluate_cp(self, feat_w, feat_b, stm):
        """Evaluates position in centipawns."""
        with torch.no_grad():
            logits = self.forward(feat_w, feat_b, stm)
            return logits * SCALE


# ==============================================================================
# Model Quantization & Binary Exporter
# ==============================================================================

def export_quantized_binary(model, output_path, qa=QA, qb=QB):
    """
    Quantizes floating-point weights to integers and exports to gc.nnue:
      1. Magic Header: 'GCNNUE02' (8 bytes)
      2. Feature Weights: int16[768][512] (786,432 bytes, feature-major)
      3. Feature Biases: int16[512] (1,024 bytes)
      4. Output Weights: int16[1024] (2,048 bytes: 0..511 STM, 512..1023 NSTM)
      5. Output Bias: int32[1] (4 bytes, scaled by QA * QB = 16320)
    Total exact size: 789,516 bytes.
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

        # Output weights: shape (1, 1024) -> (1024,)
        w_out = model.out.weight.data.squeeze(0).cpu().float()
        w_out_q = torch.clamp(torch.round(w_out * qb), -32768, 32767).to(torch.int16)

        # Output bias: shape (1,) -> scaled by QA * QB = 16320
        b_out = model.out.bias.data.squeeze(0).cpu().float()
        scale_bias = qa * qb
        b_out_q = torch.round(b_out * scale_bias).to(torch.int32)

        out_file = Path(output_path)
        out_file.parent.mkdir(parents=True, exist_ok=True)

        with open(out_file, "wb") as f:
            f.write(MAGIC_HEADER)
            f.write(w_ft_q.numpy().tobytes())
            f.write(b_ft_q.numpy().tobytes())
            f.write(w_out_q.numpy().tobytes())
            f.write(b_out_q.numpy().tobytes())

        actual_size = os.path.getsize(output_path)
        if actual_size != EXPECTED_FILE_SIZE:
            raise ValueError(
                f"Export size error: got {actual_size} bytes, expected exactly {EXPECTED_FILE_SIZE} bytes!"
            )

        print(f"\n[+] Successfully exported quantized NNUE model:")
        print(f"    File: {out_file.resolve()}")
        print(f"    Magic: {MAGIC_HEADER.decode('ascii')}")
        print(f"    Size: {actual_size:,} bytes (verified exact)")
        print(f"    Feature Weights: int16[{NUM_FEATURES}][{HIDDEN_SIZE}] ({w_ft_q.numel() * 2:,} bytes)")
        print(f"    Feature Biases:  int16[{HIDDEN_SIZE}] ({b_ft_q.numel() * 2:,} bytes)")
        print(f"    Output Weights:  int16[{OUTPUT_INPUT_SIZE}] ({w_out_q.numel() * 2:,} bytes)")
        print(f"    Output Bias:     int32[1] ({b_out_q.numel() * 4:,} bytes, scaled by {scale_bias})")


def verify_exported_binary(filepath, test_fen=None):
    """
    Verifies the binary file format, integrity, and computes a test evaluation
    using exact integer arithmetic matching the C++ engine implementation.
    """
    print(f"\n[*] Verifying binary file: {filepath}")
    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"Binary file not found: {filepath}")

    size = os.path.getsize(filepath)
    if size != EXPECTED_FILE_SIZE:
        raise ValueError(f"Size mismatch: {size} != {EXPECTED_FILE_SIZE}")

    with open(filepath, "rb") as f:
        magic = f.read(8)
        if magic != MAGIC_HEADER:
            raise ValueError(f"Magic header mismatch: {magic} != {MAGIC_HEADER}")

        w_ft_bytes = f.read(NUM_FEATURES * HIDDEN_SIZE * 2)
        b_ft_bytes = f.read(HIDDEN_SIZE * 2)
        w_out_bytes = f.read(OUTPUT_INPUT_SIZE * 2)
        b_out_bytes = f.read(4)

    w_ft = np.frombuffer(w_ft_bytes, dtype=np.int16).reshape(NUM_FEATURES, HIDDEN_SIZE)
    b_ft = np.frombuffer(b_ft_bytes, dtype=np.int16)
    w_out = np.frombuffer(w_out_bytes, dtype=np.int16)
    b_out = np.frombuffer(b_out_bytes, dtype=np.int32)[0]

    # Verify no NaN or corrupt values
    assert not np.isnan(w_ft).any(), "NaN found in feature weights"
    assert not np.isnan(b_ft).any(), "NaN found in feature biases"
    assert not np.isnan(w_out).any(), "NaN found in output weights"

    print(f"    Magic: {magic.decode('ascii')} [OK]")
    print(f"    Feature Weights: min={w_ft.min()}, max={w_ft.max()}, mean={w_ft.mean():.2f}")
    print(f"    Feature Biases:  min={b_ft.min()}, max={b_ft.max()}, mean={b_ft.mean():.2f}")
    print(f"    Output Weights:  min={w_out.min()}, max={w_out.max()}, mean={w_out.mean():.2f}")
    print(f"    Output Bias:     {b_out}")

    # Evaluate test position
    if test_fen is None:
        test_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

    w_idx, b_idx, stm = parse_fen_to_features(test_fen)

    # Integer accumulator refresh
    acc_w = b_ft.astype(np.int32) + w_ft[w_idx].astype(np.int32).sum(axis=0)
    acc_b = b_ft.astype(np.int32) + w_ft[b_idx].astype(np.int32).sum(axis=0)

    # SCReLU: clamp(x, 0, QA)^2
    y_w = np.clip(acc_w, 0, QA) ** 2
    y_b = np.clip(acc_b, 0, QA) ** 2

    y_stm = y_w if stm == 0 else y_b
    y_nstm = y_b if stm == 0 else y_w

    # Output projection in integer space
    raw = int(np.dot(y_stm, w_out[:HIDDEN_SIZE].astype(np.int64)) +
              np.dot(y_nstm, w_out[HIDDEN_SIZE:].astype(np.int64)))

    def c_div(a, b):
        return int(a / b)

    term = c_div(raw, QA) + int(b_out)
    eval_cp = c_div(term * SCALE, QA * QB)

    print(f"    Test FEN Evaluation ({'White' if stm == 0 else 'Black'} to move):")
    print(f"      FEN:  {test_fen}")
    print(f"      Raw:  {raw}")
    print(f"      Eval: {eval_cp} cp")
    print("    Binary verification passed successfully!\n")
    return eval_cp


def export_dummy_model(output_path, verify=True, test_fen=None, seed=42):
    """
    Creates an initialized dummy 512-neuron NNUE model and exports it.
    Can be used for immediate testing and C++ engine verification.
    """
    print(f"\n[*] Generating dummy 512-neuron NNUE model (seed={seed})...")
    torch.manual_seed(seed)
    np.random.seed(seed)
    model = PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE)
    export_quantized_binary(model, output_path)
    if verify:
        verify_exported_binary(output_path, test_fen=test_fen)


# ==============================================================================
# Training Pipeline
# ==============================================================================

def train_nnue(args):
    """
    Main training execution function.
    """
    # 1. Resolve Device
    if args.device == 'auto':
        device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # 2. Find Dataset
    data_path = args.data
    if data_path is None:
        # Default fallback search
        candidates = [
            "/kaggle/working/data/dataset_all.txt",
            "/kaggle/working/data",
            "data/dataset_all.txt",
            "data",
            "output/data/dataset_all.txt",
            "output/data",
            "data/selfplay.txt",
            "output/data/selfplay.txt"
        ]
        for c in candidates:
            if os.path.isfile(c) or (os.path.isdir(c) and any(Path(c).glob("*.txt"))):
                data_path = c
                break
        if data_path is None:
            raise FileNotFoundError(
                "No dataset file specified and none found in default paths "
                "(/kaggle/working/data, data/dataset_all.txt, data). "
                "Please specify using --data <filepath_or_dir>."
            )

    # 3. Load & Deduplicate Dataset
    dataset = load_dataset(
        data_path,
        max_samples=args.max_samples,
        lambda_wdl=args.lambda_wdl
    )
    total_samples = len(dataset)
    if total_samples == 0:
        raise ValueError("Dataset is empty! No valid training samples found.")

    # 4. Train / Validation Split
    val_size = int(total_samples * args.val_split)
    train_size = total_samples - val_size
    print(f"Split: {train_size:,} train samples, {val_size:,} validation samples")

    # Permute dataset
    perm = torch.randperm(total_samples)
    train_indices = perm[:train_size]
    val_indices = perm[train_size:]

    # 5. Initialize Model, Optimizer, Loss, and Scheduler
    model = PerspectiveNNUE(num_features=NUM_FEATURES, hidden_size=HIDDEN_SIZE).to(device)
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
            # Shuffle training indices each epoch
            epoch_perm = indices[torch.randperm(n_samples)]
        else:
            epoch_perm = indices

        total_loss = 0.0
        n_batches = 0
        t0 = time.time()

        for start_idx in range(0, n_samples, batch_size):
            end_idx = min(start_idx + batch_size, n_samples)
            cur_b = end_idx - start_idx
            batch_slice = epoch_perm[start_idx:end_idx]

            # Vectorized feature tensor construction using scatter_add_
            bw = dataset.w_indices[batch_slice]
            bb = dataset.b_indices[batch_slice]
            bstm = dataset.stm[batch_slice].to(device)
            btarget = dataset.targets[batch_slice].to(device)

            feat_w = torch.zeros(cur_b, NUM_FEATURES, device=device)
            feat_b = torch.zeros(cur_b, NUM_FEATURES, device=device)

            valid_w = (bw >= 0)
            feat_w.scatter_add_(1, bw.clamp(min=0).to(device), valid_w.float().to(device))

            valid_b = (bb >= 0)
            feat_b.scatter_add_(1, bb.clamp(min=0).to(device), valid_b.float().to(device))

            if is_train:
                optimizer.zero_grad()
                logits = model(feat_w, feat_b, bstm)
                loss = criterion(logits, btarget)
                loss.backward()
                optimizer.step()
            else:
                with torch.no_grad():
                    logits = model(feat_w, feat_b, bstm)
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
        type=str,
        default=None,
        help="Path to training dataset file (format: 'FEN | score | wdl' or pure FEN). "
             "Defaults to searching output/data/dataset_all.txt."
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default="gc.nnue",
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
    args, _ = parser.parse_known_args()

    if args.export_dummy:
        export_dummy_model(args.export_dummy, verify=args.verify, test_fen=args.test_fen)
        return

    if args.verify_only:
        verify_exported_binary(args.verify_only, test_fen=args.test_fen)
        return

    train_nnue(args)


if __name__ == "__main__":
    main()
