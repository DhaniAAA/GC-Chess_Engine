# GC-Chess-Engine — Agent Guide

## Build (Windows/MinGW)

- **Standard release:** `mingw32-make all` (or just `mingw32-make`)
- **PEXT/BMI2 (modern CPUs):** `mingw32-make pext`
- **Debug:** `mingw32-make debug`
- **PGO (2-step):** `mingw32-make pgo-generate` → run bench → `mingw32-make pgo-use`
- **PEXT + PGO (max perf):** `mingw32-make pext-pgo-generate` → run bench → `mingw32-make pext-pgo-use`
- **Internal profiler:** `mingw32-make internal-profile` (enables `-DPROFILING`; run `bench` for results)
- **Texel tuner:** `mingw32-make tuner` → `output\tuner.exe tuner\quiet-labeled.epd`
- **Output:** `output/main.exe` (Windows) / `output/main` (Linux)

## Tests

- **Unit tests:** `mingw32-make run-tests` (custom framework in `tests/test_main.cpp`, NOT catch2). `test_main.cpp` `#include`s the other `test_*.cpp` files.
- **Unit test files:** `test_types/bitboard/move/board/movegen/tt` (original) + `test_perft_deep` (CPW perft d4–d6), `test_draws` (repetition/50-move/insufficient material), `test_see` (suite see-test.epd embedded), `test_eval` (symmetry & sanity). New test file = create `tests/test_X.cpp` + `#include "test_X.cpp"` in `test_main.cpp` (no Makefile change needed; `src/` wildcard only picks up `src/*.cpp`).
- **Integration tests (search/waktu):** `python tests/run_integration.py [engine]` — mate detection, determinism, movetime limits, `go depth N nodes M` node-limit regression, extreme time pressure.
- **Time-guard (anti time-forfeit):** `python tests/run_time_guard.py [engine]` — 16 posisi × 3 tier jam ketat; FAIL pada overshoot/hang/tanpa bestmove.
- **Forfeit scanner:** `python tests/check_forfeits.py hasil.pgn` — scan PGN cutechess untuk "wins on time".
- **Baseline regression:** `tests/baseline.txt` — catat bench nodes, WAC & BK skor; bandingkan setelah ubah search.
- **Explicit source lists:** the main build uses a wildcard for `src/*.cpp`, but the test and tuner builds use fixed lists in the Makefile (`ENGINE_SOURCES_FOR_TESTS`, `TUNER_SOURCES`). A new `src/*.cpp` builds into `main` automatically but will NOT be linked into `run_tests`/`tuner` until added to those lists. NOTE: `src/search.cpp` is NOT in `ENGINE_SOURCES_FOR_TESTS` — Search-dependent tests must go through `run_integration.py` (UCI pipe), not the unit framework.
- **Perft:** `echo "perft 5" | .\output\main.exe`
- **Benchmark:** `echo "bench" | .\output\main.exe` (default: depth 13, 1 thread, 16MB hash)
- **Custom bench:** `echo "bench 15 4 256" | .\output\main.exe` (depth 15, 4 threads, 256MB)
- **Test suites:** `tests/wac.epd`, `tests/bratko_kopec.epd`, `tests/see-test.epd`
- **Gauntlet/SPRT:** `run_gauntlet.bat` or `run_sprt.bat` (uses `cutechess/cutechess-cli.exe`, engines in `engines/`)

## Architecture

- `src/main.cpp` — entrypoint → `init_engine()` then `UCI::UCIHandler::loop()`
- **Build source:** Only flat `src/*.cpp` is compiled on Windows (`wildcard src/*.cpp`). Subdirectories `src/core/`, `src/search/`, `src/eval/`, `src/uci/`, `src/tools/`, `src/utils/` mirror those files and are NOT in the build — **edit the flat `src/*.cpp`, not the subdir copies** (last-write-wins confusion is the top risk here).
- **Linux caveat:** the Makefile's non-Windows branch globs `src` recursively (`find src -type d`), so it would compile the mirror copies too → duplicate symbols. Windows/MinGW is the supported build path.
- **Headers:** All 24 `.hpp` files in `include/` (flat, no subdirs)
- **Tests excluded from main build:** `tests/test_main.cpp`, `test_types.cpp`, `test_bitboard.cpp`, `test_move.cpp`, `test_board.cpp`, `test_movegen.cpp`, `test_tt.cpp`. The `tests` target now also depends on `$(TEST_DIR)/test_*.cpp`, so editing test files always triggers a relink.

## UCI Non-Standard Commands

| Command | Description |
|---------|-------------|
| `perft N` | Perft test to depth N |
| `divide N` | Divided perft |
| `d` | Display board |
| `eval` | Static evaluation |
| `bench [depth] [threads] [hash]` | Benchmark |
| `wac [depth N] [time N] [start N end N] [quiet]` | WAC tactical suite (299 posisi) |
| `bk [depth N] [time N] [quiet]` | Bratko-Kopec strategic suite (24 posisi, sensor regression) |
| `datagen <subcommand>` | Training data generation (binpack format). Subcommands: `start`, `stop`, `status`, `filter`, `view`, `stats`, `convert` |

## Tuners & Tools

- **Texel tuner:** `mingw32-make tuner` → `output\tuner.exe tuner\quiet-labeled.epd` (usage: `tuner.exe <epd_file> [max_positions] [iterations]`)
- **SPSA tuner (Python):** `tuner/spsa_tuner.py` — supports simple mode and cutechess full-game mode
- **NNUE trainer:** `nnue_trainer/` (Kaggle notebooks + Python scripts)
- **CI tuning:** `.github/workflows/texel-tuner.yml` and `spsa-tuning.yml` — manual dispatch only, runs on Ubuntu

## Performance Hot Spots

Priority files for engine tuning:
- `src/search.cpp` — `Search()` function
- `src/eval.cpp` — `Evaluate()` function
- `src/moveorder.cpp` — move ordering (MVV-LVA, SEE, killers, history, continuation history)
- `src/movegen.cpp` — move generation
- `src/tt.cpp` — transposition table
- `include/optimize.hpp` — `FORCE_INLINE`, `CACHE_ALIGNED`, `LIKELY`/`UNLIKELY`, `PREFETCH_*`

## Gotchas

- PGO is a **2-step** process: generate (instrumented binary) → run benchmarks → use (final optimized binary)
- CI workflows are **manual dispatch only** (no push/PR triggers)
- `tests/catch2/` contains a catch2 header but the test framework is **custom**, defined in `test_main.cpp`
- Makefile uses `-MMD` for auto-dependency tracking; stale `.d` files can cause rebuild issues — run `mingw32-make clean` if you get odd errors
- Default release build uses `-flto -funroll-loops -march=native` — link-time optimization means full rebuilds are slower but runtime is faster
- Makefile: target `tests` sudah `.PHONY` — dulu tertutupi oleh direktori `tests/` sehingga relink tidak terjadi meski file test berubah (bug menyebabkan exe test basi)
- `src/test_eval.cpp` ada di `EXCLUDES` (punya `main()` sendiri, bukan bagian engine)
