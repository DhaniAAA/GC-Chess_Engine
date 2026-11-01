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

<!-- scopecairn:managed -->
# ScopeCairn — Automated Engineering Protocol

## 1. Discipline (Anti-Slop & Laziness Protocol)
- **Minimal Diff**: Smallest change that solves the problem; bias to deletion; no unrequested abstractions or narrative step comments.
- **Boundaries & History**: Validate at I/O/CLI boundaries; keep core logic pure; check git blame before deleting unusual edge-case guards.

## 2. Context Retrieval & Status Cue
- **First line of EVERY response**:
  - Called context: `> 🧭 [ScopeCairn: context "<query>" → N files in scope]`
  - Skipped context: `> 🧭 [ScopeCairn: scope known (<reason>), context skipped]`
  - Checked impact / verified: `> 🧭 [ScopeCairn: impact <path> checked]` / `> 🧭 [ScopeCairn: verify passed within scope]`
- **Cold start**: If `.scopecairn/scopecairn.db` is missing, run `scopecairn init`. At the start of a session, run `scopecairn session load` to restore previous working state and git drift.
- **Run `scopecairn context "<2-4 technical keywords>"` ONLY IF** target files/symbols are unknown or locking edit scope. If results are empty/off-target, run `scopecairn map [dir]` then `scopecairn context --anchor <path> "<query>"`.
- **SKIP `scopecairn context` IF**: (1) user named the target file/symbol, (2) prompt is Q&A/architecture/review, or (3) scope is already known in this session.

## 3. Execution & Verification
- **Scope**: Edit only Required/Optional files; never touch Protected without approval. If >2 files change, run `scopecairn context --escalate "<query>"`.
- **Reuse Check**: Before creating a new function or class, run `scopecairn reuse "<description or signature>"` and reuse/extend existing symbols when matched.
- **Shared Symbols**: Run `scopecairn impact <path>`, state 1 safety fact, and prove it via `scopecairn impact <path> --run-tests`.
- **Anti-Looping**: 2 failed fixes on the same test → stop and test the root-cause premise independently before a 3rd edit.
- **Excerpts**: Prefer `scopecairn read symbol <name>` or line-ranged reads from `context` over full-file reads.
- **Before Done**: Self-review edge/async/OS cases, prove with tests (never weaken tests by removing assertions, adding `.skip`, or loosening matchers), then run `scopecairn scan`, `scopecairn dead`, `scopecairn verify --task "<query>" --strict --test-gate`, and `scopecairn session save --working "<summary>" --decisions "<decisions>" --next "<next>" --blockers "<blockers>" --files "<files>"`. Never bypass git pre-commit hooks with `--no-verify`; teams should also enforce `scopecairn verify --test-gate --base origin/main` in CI.
- **Decisions**: Record settled architectural choices via `scopecairn decide "<choice> because <why>"` — stored with evidence and included in all future contexts.
