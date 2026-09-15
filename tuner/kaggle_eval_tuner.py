#!/usr/bin/env python3
"""
============================================================================
GC-Chess Engine - Eval Tuner (SPSA over UCI options, engine-in-the-loop)
============================================================================
Tunes the engine's evaluation parameters against tuner/quiet-labeled.epd
(1.4M quiet positions with game results) by minimizing Texel error.

HOW IT WORKS:
  * Keeps ONE engine process alive for the whole run (fast: ~9K evals/sec).
  * Sends `setoption` for each candidate parameter set, then streams
    `position fen ...` / `eval` and reads back static evals.
  * Loss = mean squared error between game result and
    sigmoid(K * eval_white_pov), with K auto-selected by line search.
  * Optimizer = SPSA (only 2 error evaluations per step, scales to all
    params at once) with elitism (best-so-far is kept and reported).

PARAMETERS TUNED (13 — every eval tunable exposed as a UCI option):
  Pawn/Knight/Bishop/Rook/Queen Value MG+EG (10), RookOpenFileBonus MG+EG (2),
  KingSafetyWeight (1).

GOTCHAS (verified against this engine, do not "simplify" away):
  * `eval` prints SIDE-TO-MOVE perspective -> flip sign when Black to move.
  * Material/PST are snapshotted at `position` time -> this script ALWAYS
    sends all `setoption` lines BEFORE the positions of each pass.
  * quiet-labeled.epd is gitignored (tuner/*.epd) -> NOT on GitHub. For
    Kaggle, upload it as a Dataset and pass --epd /kaggle/input/... .

USAGE (local smoke test, ~2 min):
  python tuner/kaggle_eval_tuner.py --engine output/main.exe --positions 3000 --iters 15

USAGE (Kaggle, CPU notebook, accelerator None):
  !git clone --depth 1 https://github.com/DhaniAAA/GC-Chess_Engine.git
  !cd GC-Chess_Engine && make clean && make all
  !python GC-Chess_Engine/tuner/kaggle_eval_tuner.py \
      --engine GC-Chess_Engine/output/main \
      --epd /kaggle/input/<your-dataset>/quiet-labeled.epd \
      --positions 100000 --iters 250 --out /kaggle/working/tuned.json

APPLYING RESULTS:
  1. The script prints a tuning.cpp-ready snippet + writes --out JSON.
  2. Paste values into src/tuning.cpp, rebuild, re-run WAC/bench/SPRT.
  3. Tuned values are a starting point: validate playing strength via
     run_sprt.bat — lower Texel error usually helps, but SPRT decides.
============================================================================
"""

import argparse
import json
import math
import os
import random
import subprocess
import sys
import time

# (uci_option, init_value, min_bound, max_bound)
PARAMS = [
    ("PawnValueMG", 70, 0, 2000),
    ("PawnValueEG", 80, 0, 2000),
    ("KnightValueMG", 293, 0, 2000),
    ("KnightValueEG", 300, 0, 2000),
    ("BishopValueMG", 290, 0, 2000),
    ("BishopValueEG", 300, 0, 2000),
    ("RookValueMG", 450, 0, 2000),
    ("RookValueEG", 470, 0, 2000),
    ("QueenValueMG", 880, 0, 5000),
    ("QueenValueEG", 900, 0, 5000),
    ("RookOpenFileBonusMG", 5, 0, 500),
    ("RookOpenFileBonusEG", 0, 0, 500),
    ("KingSafetyWeight", 90, 0, 200),
]

K_CANDIDATES = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]


def sigmoid(k, score_cp):
    return 1.0 / (1.0 + 10.0 ** (-k * score_cp / 400.0))


def load_epd(path, n, seed=12345):
    data = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "c9" not in line:
                continue
            try:
                fen_part, res_part = line.split(" c9 ", 1)
                res = res_part.strip().strip(";").strip().strip('"')
                if res == "1-0":
                    r = 1.0
                elif res == "0-1":
                    r = 0.0
                elif res in ("1/2-1/2", "1/2"):
                    r = 0.5
                else:
                    continue
                stm = fen_part.split(" ")[1]
                data.append((fen_part.strip(), r, stm))
            except (ValueError, IndexError):
                continue
    rnd = random.Random(seed)
    rnd.shuffle(data)
    return data[:n] if n > 0 else data


class Engine:
    """Persistent engine process speaking UCI."""

    def __init__(self, exe):
        self.proc = subprocess.Popen(
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self._send("uci")
        self._wait_for("uciok")
        self._send("setoption name Threads value 1")
        self._send("setoption name Hash value 16")
        self._send("isready")
        self._wait_for("readyok")

    def _send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def _readline(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("engine died (EOF on stdout)")
        return line

    def _wait_for(self, token):
        while True:
            if token in self._readline():
                return

    def set_params(self, names, values):
        for name, v in zip(names, values):
            self._send(f"setoption name {name} value {int(v)}")

    def eval_white_pov(self, fen, stm):
        self._send(f"position fen {fen}")
        self._send("eval")
        score = None
        for _ in range(8):  # consume until Evaluation line (skip stray info)
            line = self._readline()
            if line.startswith("Evaluation:"):
                try:
                    score = int(line.split()[1])
                except (IndexError, ValueError):
                    score = 0
                break
        self._readline()  # consume "Side to move:" line to stay in sync
        if score is None:
            raise RuntimeError("no Evaluation line from engine")
        return score if stm == "w" else -score

    def error(self, dataset, names, values, k):
        self.set_params(names, values)
        total = 0.0
        for fen, result, stm in dataset:
            s = self.eval_white_pov(fen, stm)
            total += (result - sigmoid(k, s)) ** 2
        return total / len(dataset)

    def close(self):
        try:
            self._send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def clamp(v, lo, hi):
    return max(lo, min(hi, int(round(v))))


def main():
    ap = argparse.ArgumentParser(description="SPSA eval tuner (engine-in-the-loop)")
    ap.add_argument("--engine", required=True)
    ap.add_argument("--epd", required=True)
    ap.add_argument("--positions", type=int, default=100000)
    ap.add_argument("--iters", type=int, default=250)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--k", type=float, default=0.0, help="0 = auto line-search")
    ap.add_argument("--out", default="tuned_eval.json")
    args = ap.parse_args()

    names = [p[0] for p in PARAMS]
    theta0 = [float(p[1]) for p in PARAMS]
    bounds = [(p[2], p[3]) for p in PARAMS]
    scales = [max(abs(v), 10.0) for v in theta0]

    print(f"Loading EPD: {args.epd}")
    data = load_epd(args.epd, args.positions, args.seed)
    print(f"  {len(data)} positions ready")
    if len(data) < 100:
        sys.exit("ERROR: too few usable positions — check --epd path/format.")

    eng = Engine(args.engine)
    try:
        # ---- Phase 1: K line search ----
        if args.k > 0:
            best_k = args.k
            print(f"Using manual K = {best_k}")
        else:
            print("Phase 1: K line search...")
            base = [clamp(v, *b) for v, b in zip(theta0, bounds)]
            best_k, best_e = None, float("inf")
            for k in K_CANDIDATES:
                e = eng.error(data, names, base, k)
                print(f"  K={k:<5} error={e:.6f}")
                if e < best_e:
                    best_e, best_k = e, k
            print(f"  -> best K = {best_k} (error {best_e:.6f})")

        # ---- Phase 2: SPSA ----
        print(f"Phase 2: SPSA ({args.iters} iters, {len(names)} params)...")
        x = [t / s for t, s in zip(theta0, scales)]  # normalized space
        cur = [clamp(v, *b) for v, b in zip(theta0, bounds)]
        best = list(cur)
        best_e = eng.error(data, names, best, best_k)
        print(f"  iter -1  error={best_e:.6f} (initial)")

        A = max(10, args.iters // 10)
        a0, c0 = 0.25, 0.05
        alpha, gamma = 0.602, 0.101
        rnd = random.Random(args.seed + 1)
        t0 = time.time()

        for k in range(args.iters):
            ak = a0 / (A + k + 1) ** alpha
            ck = c0 / (k + 1) ** gamma
            delta = [1.0 if rnd.random() < 0.5 else -1.0 for _ in names]
            xp = [xi + ck * d for xi, d in zip(x, delta)]
            xm = [xi - ck * d for xi, d in zip(x, delta)]
            tp = [clamp(xi * s, *b) for xi, s, b in zip(xp, scales, bounds)]
            tm = [clamp(xi * s, *b) for xi, s, b in zip(xm, scales, bounds)]
            ep = eng.error(data, names, tp, best_k)
            em = eng.error(data, names, tm, best_k)
            for i in range(len(x)):
                g = (ep - em) / (2.0 * ck)
                g = max(-1.0, min(1.0, g))  # gradient clip for stability
                x[i] -= ak * g * delta[i]
            cur = [clamp(xi * s, *b) for xi, s, b in zip(x, scales, bounds)]
            ec = eng.error(data, names, cur, best_k)
            if ec < best_e:
                best_e, best = ec, list(cur)
            if (k + 1) % 10 == 0 or k == 0:
                dt = time.time() - t0
                print(f"  iter {k + 1:<5} cur_err={ec:.6f} best_err={best_e:.6f} "
                      f"elapsed={dt:.0f}s")
    finally:
        eng.close()

    result = {
        "k": best_k,
        "error": best_e,
        "positions": len(data),
        "iters": args.iters,
        "params": {n: int(v) for n, v in zip(names, best)},
    }
    with open(args.out, "w") as f:
        json.dump(result, f, indent=2)

    print("\n" + "=" * 60)
    print(f"BEST error={best_e:.6f}  K={best_k}")
    print("tuning.cpp-ready snippet:")
    for n, v in zip(names, best):
        mg_eg = "mg" if n.endswith("MG") else ("eg" if n.endswith("EG") else "")
        print(f"  {n} = {int(v)}")
    print(f"\nFull JSON saved to: {args.out}")
    print("Next: paste into src/tuning.cpp -> rebuild -> WAC/bench/SPRT.")


if __name__ == "__main__":
    main()
