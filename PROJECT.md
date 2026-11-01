# Project: GC-Chess-Engine WAC Performance Optimization

## Architecture
- Module/package boundaries: flat `src/*.cpp` and `include/*.hpp`.
- Key search subsystems:
  - `src/search.cpp` / `include/search.hpp`: PVS, iterative deepening, root move ordering, LMR, extensions, pruning, history updates.
  - `src/moveorder.cpp` / `include/moveorder.hpp`: MovePicker, stage transitions, capture scoring, quiet scoring, SEE.
  - `src/eval.cpp` / `include/eval.hpp`: Static evaluation, PSQT, material, pawns, king safety.
  - `src/tt.cpp` / `include/tt.hpp`: Transposition table.
- Data flow: UCI commands -> Position/Search -> MoveGen/MoveOrder -> Search/Eval/TT -> Bestmove.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Standard PVS Root Alpha Update | Fix `src/search.cpp:1897-1925` so non-PV moves (`moveCount > 1`) only update `bestScore` if `score > alpha && score > bestScore`, removing `(rootNode && score == alpha)` and the check tiebreaker. Solves WAC.063, 093, 144, 157, 231, 300 | M1 | Explorers 1, 3 (VERIFIED) |
| 2 | Rank 6 Passed Pawn Extension Scale | Fix integer division in `src/search.cpp:1552` by setting `FRAC_EXT_PASSED_PAWN` (100) instead of `/ 2`. | M1 | Explorer 1 (VERIFIED) |
| 3 | Root LMR Elimination | In `src/search.cpp:1740-1742`, set `if (rootNode) reduction = 0;`. Solves WAC.196, 243 | M1 | Explorers 2, 3 (VERIFIED) |
| 4 | Rank 5 Passed Pawn Push Exemption | In `src/search.cpp:1408` and `1648`, extend `isAdvancedPawnPush` to include relative rank 5 passed pawns. | M1 | Explorers 2, 3 (VERIFIED) |
| 5 | Full WAC Suite Depth 10 Verification | Run `python tests/wac_run.py 10 300` to verify engine achieves >= 293/300 solved positions (Achieved 293/300) | M1 | ORIGINAL_REQUEST (VERIFIED) |
| 6 | Movegen and Benchmark Verification | Verify `echo "perft 5" | .\output\main.exe` (4,865,609 nodes) and `echo "bench" | .\output\main.exe` (17,358,175 nodes) | M1 | AGENTS.md (VERIFIED) |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Search Correctness & WAC Tactical Fixes | Implement Features 1-4 in `src/search.cpp`; verify Features 5-6 | none | DONE |

## Interface Contracts
- `Search::search()` PVS Contract: Strict alpha-beta invariants: a move only raises alpha if `score > alpha`. Fail-low scores from null-window probes must never overwrite `bestScore`, `bestMove`, or `rootMoves[0]`.
- `Search::start()` contract: Fully thread-safe, manages root search lifecycle, maintains UCI option compatibility.
- `Board` & `Eval` contract: Pure, const-correct evaluation functions without thread-unsafe global mutations.

## Code Layout
- `src/search.cpp`: PVS root move alpha updates, LMR root exemption, passed pawn extensions, rank 5 push exemption, stability inertia.
- `output/main.exe`: Target executable built via `mingw32-make`.
