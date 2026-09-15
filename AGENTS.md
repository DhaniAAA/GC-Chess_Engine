# GC-Chess-Engine — Agent Guide

## Build (Windows/MinGW is primary; Linux supported)

- `mingw32-make` (or `mingw32-make all`) — standard release (`-O3 -flto -funroll-loops -march=native`, `-static`)
- `mingw32-make pext` — BMI2 build, only for Haswell+/Zen 3+ with 64-bit MinGW
- `mingw32-make debug` — `-O0 -g`, for GDB
- PGO is 2-step: `mingw32-make pgo-generate` → run `echo "bench" | .\output\main.exe` → `mingw32-make pgo-use` (same with `pext-pgo-*` for max perf)
- `mingw32-make internal-profile` — enables `-DPROFILING`; run `bench` to see results
- `mingw32-make run` / `mingw32-make clean` — run / full rebuild
- Output: `output/main.exe` (Windows) / `output/main` (Linux). Linux links `-pthread`.
- Stale `.d` rebuild errors → `mingw32-make clean` first (`-MMD` dep tracking; `.o`/`.d` litter in `src/` is gitignored, as is `output/`).

## Layout

- `src/main.cpp` — entrypoint: `init_engine()` (`Position::init()` covers Bitboards/Magics/Zobrist) → `UCI::UCIHandler::loop()`
- Flat `src/*.cpp` (15 files) + flat `include/*.hpp` (21 files). Mirror subdirs (`src/core/`, `src/search/`, …) were deleted in HEAD — do not recreate them.
- Hot spots for tuning: `src/search.cpp` (`Search()`), `src/eval.cpp` (`Evaluate()`), `src/moveorder.cpp`, `src/movegen.cpp`, `src/tt.cpp`, `include/optimize.hpp`.

## Verify

- `echo "perft 5" | .\output\main.exe` — movegen correctness
- `echo "bench" | .\output\main.exe` — default depth 13, 1 thread, 16MB hash; `echo "bench 15 4 256" | .\output\main.exe` for custom
- `python tests/wac_run.py [depth] [max_pos]` — WAC suite over UCI pipe (requires `python-chess`, engine at `output\main.exe`, EPD at `tests\wac.epd`; misses → `tests\wac_miss.txt`)
- `run_gauntlet.bat` / `run_sprt.bat` — cutechess gauntlet/SPRT (`cutechess/cutechess-cli.exe`, opponents in `engines/`)

## UCI extras (verified in `src/uci.cpp:loop()`)

`perft N`, `divide N`, `d`, `eval`, `bench [depth] [threads] [hash]`, plus standard `uci/isready/ucinewgame/position/go/stop/ponderhit/quit/setoption`.

## Stale docs — do not trust

- `tests/baseline.txt` and `Build-project.md` reference `wac`/`bk`/`datagen` engine commands and `tests/run_integration.py`, `tests/run_time_guard.py`, `mingw32-make run-tests`/`tuner` — all deleted from this tree (no `test_*.cpp`, no `ENGINE_SOURCES_FOR_TESTS`, no tuner target in Makefile). There is currently **no unit-test target**; verify via `perft`/`bench`/`wac_run.py`/gauntlet.
- `BUILD.md` "Build Tuner" section is stale for the same reason. Tuner data still present (`tuner/quiet-labeled.epd`, `tuner/kaggle_texel_tuner.py`) but no Makefile target builds it.
- CI workflows (`.github/workflows/`) are `workflow_dispatch` only (manual).
