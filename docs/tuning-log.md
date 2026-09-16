# Tuning Log — WAC Tactics Push (Sept 2026)

Goal was 100% WAC. Final verdict: **accepted 200/300 @depth-10** (from 192),
250/300 @5s movetime. 100% @fixed-depth-10 deferred: 19 controlled experiments
show this engine at a local optimum — nearly every search relaxation *lowers*
the score. Remaining misses need structural eval work (threat awareness),
not knob tuning.

## Verified final state (binary = HEAD)

- `perft 5` → 4865609 nodes
- `python tests/wac_run.py 10` → 200/300
- `bench` (default 13/1/16MB) → 2613675 nodes
- Tree clean, binary = committed `output/main.exe`.

## Baseline journey

| Step | WAC10 | Notes |
|---|---|---|
| Initial build (v1.2.1, stale .o cleaned) | 192/300 | Below historical 218 (baseline.txt) — real regression from search changes |
| Fix corrupt EPD WAC.274 (`am Rd6` → valid FEN) | +1 solvable | tests/wac.epd:274 |
| Depth scaling (diagnostic): d12 → 225, d14 → 241 | — | 59 misses structural even @d14 |
| Time-based 5s/pos | 250/300 | Standard WAC methodology scores much higher |

## Search-knob experiments (all: rebuild + WAC10 + bench)

Kept (in `search_constants.hpp` / `search.cpp`, with comments):
- Revert deepened pruning (futility 11→8, RFP/LMP 12→9, SEE-cap 9→6, SEE-quiet 8→5): **+1**
- LMR tactical-quiet discount −2 → −3: **+3**
- Singular depth 8 → 6: **+4**

Rejected (reverted, reasons recorded in code comments):
- Mate-threat ext 0.75→1.0: −1, +54% bench nodes
- Qsearch checks to 2nd ply: neutral, +15% nodes
- LMP/futility bypass for tactical-quiet: **−10**
- Root LMR cap ≤1: −9, +46% nodes
- Tactical-quiet ordering bonus 5K→20K: −3
- Capture-ext depth 10→8: −4 (cascade extensions)
- Recapture ext 0.5→1.0: −2, +31% nodes
- Delta margin 350→500: **−16** (unexpected interaction, do not touch qsearch)
- Futility margin 150→120: −15 (more aggressive); 150→180: −6 (150 is local optimum)
- RFP margin 75→100: −4, +44% nodes
- ProbCut margin 200→300: neutral
- NMP base R 3→2: −5, +43% nodes
- MultiCut off: neutral
- Singular margin 80→60: neutral
- Diagnostic LMR-off: only +5 → LMR is NOT the main killer

## Kaggle eval tuning (Texel/SPSA, `tuner/kaggle_eval_tuner.py`)

- Round 1 (100K pos, 13 params): err 0.072277→0.071171. Round 2 (528K pos,
  36 params): err 0.072035→0.070961. Both runs agreed qualitatively
  (knight down, bishop/queen up) → tuner validated, not noise.
- **Applied values REVERTED**: WAC10 dropped 200→191 (2× confirmed) and bench
  +35%. Lesson, proven with our own data: Texel error (static-eval fit on quiet
  positions) ≠ tactical strength. New material scale shifted fixed-cp pruning
  margins. SPRT/WAC decide, never the fitting error.
- King-safety params did not move in either run (2×528K+100K positions agree
  they are locally optimal for this objective). Qh6-type blindness is
  structural (missing threat term), not a weight problem.

## Repro

- WAC: `mingw32-make all` (clean first on LTO mismatch) →
  `python tests/wac_run.py 10` (needs `python-chess`).
- Time-based: `tuner/` script `wac_time.py` was a temp file (not in repo);
  recreate if needed: same as wac_run.py with `go movetime <ms>`.
- Kaggle: upload `tuner/quiet-labeled.epd` (gitignored, 80MB, 1.428M pos) as
  Dataset → CPU notebook, accelerator None → clone, `make`, run
  `tuner/kaggle_eval_tuner.py --positions 100000 --iters 250 --jobs 0`.

## Gotchas discovered (do not re-learn)

- `eval` prints **side-to-move** POV → flip when Black to move.
- Material/PST snapshotted at `position` time → tuner MUST send `setoption`
  BEFORE positions each pass (verified: QueenValueMG 880→2000 changes eval
  only in that order).
- `quiet-labeled.epd` is gitignored → never on GitHub; Kaggle needs Dataset.
- Stale `.o` with LTO version mismatch (13.1 vs 15.1) → `mingw32-make clean`
  first. Seen twice (something rebuilds with another MinGW).
- `moves`-vs-`fen` eval scare was a **false alarm** (hand-written FEN put the
  queen on h4 with a phantom second queen). Bulk-checked 975 positions
  (captures, both castles, EP, promotions, WAC walks): 0 mismatches.

## Open directions (not started)

1. **Threat-awareness eval term** (structural): Qh6 (correct tactic) evals −62
   vs +114 initial — eval doesn't price mate threats. Needs component
   breakdown → new term → perft/WAC/bench/SPRT validation.
2. SPSA from games (`tuner/spsa_tuner.py` + cutechess workflow): optimizes Elo
   directly, immune to the proxy trap. Expensive (hundreds of games).
3. Constrained Texel: anchor pawn=70/80, tune rest relatively, so fixed-cp
   pruning margins don't shift. Only if curious.
