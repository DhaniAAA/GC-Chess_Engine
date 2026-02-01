# AGENTS.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview
GC-Engine is a UCI-compliant chess engine written in C++17 that implements advanced search algorithms (alpha-beta with PVS, quiescence search), sophisticated evaluation (tapered eval with PST, pawn structure, king safety, threats), and modern techniques like Lazy SMP multithreading, NNUE training support, and Texel tuning.

## Build Commands

### Windows (MinGW)
```bash
# Standard release build (default)
mingw32-make clean & mingw32-make all

# PEXT build (Intel Haswell+, AMD Zen 3+) - faster bitboard operations
mingw32-make clean & mingw32-make pext

# Profile-Guided Optimization (two-step process)
# Step 1: Generate profiling data
mingw32-make clean & mingw32-make pgo-generate
echo "perft 6" | .\output\main.exe
echo "bench" | .\output\main.exe
# Step 2: Build with profile data
mingw32-make pgo-use

# Maximum performance: PEXT + PGO
mingw32-make clean & mingw32-make pext-pgo-generate
echo "perft 6" | .\output\main.exe
mingw32-make pext-pgo-use

# Debug build (no optimization, with symbols)
mingw32-make clean & mingw32-make debug

# Internal profiling build (enables PROFILE_SCOPE macros)
mingw32-make internal-profile

# Texel tuner build
mingw32-make tuner
```

### Linux
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update && sudo apt-get install -y g++ make

# Standard release build
make clean && make all

# PEXT build (modern CPUs)
make clean && make pext

# Use Clang instead of GCC
make CXX=clang++ clean && make CXX=clang++ all
```

### Build Output
- Windows: `output/main.exe`
- Linux: `output/main`
- Tuner: `output/tuner.exe` or `output/tuner`
- Unit Tests: `output/run_tests.exe` or `output/run_tests`

## Testing Commands

### Perft Testing (Move Generation Verification)
```bash
# Quick perft tests
echo "perft 5" | .\output\main.exe
echo "divide 5" | .\output\main.exe

# Expected results:
# perft 5: 4,865,609 nodes (~1.3s)
# perft 6: 119,060,324 nodes (~74s)
```

### Built-in Benchmark
```bash
# Default benchmark (depth 13, 1 thread, 16MB hash)
echo "bench" | .\output\main.exe

# Custom benchmark
echo "bench 15 4 256" | .\output\main.exe  # depth 15, 4 threads, 256MB hash
```

### Unit Tests
```bash
# Build and run unit tests
mingw32-make tests
.\output\run_tests.exe

# Or build and run in one step
mingw32-make run-tests
```

### Test Suites (EPD Files)
Test position files are located in `tests/`:
- `wac.epd` - Win at Chess tactical positions
- `bratko_kopec.epd` - Bratko-Kopec test suite
- `see-test.epd` - Static Exchange Evaluation tests

### Engine vs Engine Testing
```bash
# Run gauntlet test against multiple opponent engines
.\run_gauntlet.bat

# Run SPRT (Sequential Probability Ratio Test) for strength testing
.\run_sprt.bat
```

## UCI Commands Reference

### Standard UCI Commands
```bash
uci                     # Display engine info and options
isready                 # Check if engine is ready
ucinewgame              # Start new game (clear hash/history)
position startpos       # Set to starting position
position fen <fen>      # Set to specific FEN
go depth 10             # Search to depth 10
go movetime 5000        # Search for 5 seconds
stop                    # Stop current search
quit                    # Exit engine
```

### Non-Standard Commands (for debugging)
```bash
perft N                 # Run perft to depth N
divide N                # Run divided perft to depth N
d                       # Display current board
eval                    # Show static evaluation
bench [depth] [threads] [hash]  # Run benchmark
```

## Code Architecture

### Core Components

**Board Representation (`board.hpp`, `board.cpp`)**
- `Board` class: Central chess position representation
- `StateInfo`: Captures all state needed to unmake moves (hash keys, castling rights, en passant, checkers, pinners, check squares)
- Incremental updates: Zobrist keys and PST scores updated during move making/unmaking
- Attack detection: `attackers_to()`, `is_attacked_by()`, `slider_blockers()` for pins

**Bitboards (`bitboard.hpp`, `bitboard.cpp`)**
- Pre-calculated attack tables for pawns, knights, kings
- Shift operations with edge wrapping handled correctly
- Line/between bitboards for sliding piece logic
- `popcount()`, `lsb()`, `msb()`, `pop_lsb()` primitives

**Magic Bitboards (`magic.hpp`, `magic.cpp`, `pext.hpp`)**
- Fancy magic bitboards for rook/bishop attack generation
- PEXT/BMI2 support via compile-time flag (`-DUSE_PEXT`)
- Magic constants and attack tables initialized at startup

**Move Representation (`move.hpp`)**
- 16-bit encoding: 6 bits from-square, 6 bits to-square, 2 bits flags, 2 bits promotion
- `MoveType`: NORMAL, PROMOTION, EN_PASSANT, CASTLING
- `MoveList`: Fixed-size array with partial sorting via `pick_best()`
- `ScoredMove`: Move + score pair for move ordering

**Move Generation (`movegen.hpp`, `movegen.cpp`)**
- Templated generation: ALL, CAPTURES, QUIETS, EVASIONS, LEGAL, CHECKING
- Separate pawn move generation (pushes, captures, promotions, en passant)
- Castling validation (not through check, not into check, squares empty)
- Legal move filtering via `is_legal()` checks for pins and discovered checks

**Move Ordering (`moveorder.hpp`, `moveorder.cpp`)**
- Static Exchange Evaluation (SEE) for capture analysis
- History heuristics: main history, continuation history (1-ply, 2-ply), capture history
- Killer moves (2 per ply) and countermove heuristic
- MVV-LVA for initial capture ordering
- `MovePicker` class orchestrates staging: TT move → captures → killers → quiets

**Search (`search.hpp`, `search.cpp`)**
- Iterative deepening with aspiration windows
- Principal Variation Search (PVS) / Alpha-Beta
- Quiescence search with delta pruning
- Multi-PV support for analysis
- `SearchStack`: Per-ply information (static eval, killers, PV, extensions, move count)
- `CorrectionHistory`: Tracks eval vs search score differences to correct systematic bias

**Evaluation (`eval.hpp`, `eval.cpp`)**
- Tapered evaluation: Interpolate between middlegame/endgame scores
- Material + Piece-Square Tables (PST) updated incrementally in Board
- Pawn structure: passed pawns, isolated, doubled, backward, phalanx
- Piece evaluation: mobility, outposts, bishop pair, rook on open files
- King safety: attack units, pawn shield, king ring attacks
- Threats: piece threats, hanging pieces
- `EvalContext`: Caches attack maps to avoid redundant calculations

**Transposition Table (`tt.hpp`, `tt.cpp`)**
- Multi-bucket hash table (4 entries per bucket)
- `TTEntry`: Stores hash key, best move, score, depth, bound type, generation
- Replacement scheme prioritizes higher depth and current generation
- Score adjustment for mate distances (`score_to_tt()`, `score_from_tt()`)

**Threading (`thread.hpp`, `thread.cpp`)**
- Lazy SMP: Multiple threads search same position with shared TT
- `ThreadPool`: Manages worker threads
- `SearchThread`: Per-thread state (history tables, stack, stats)

**UCI Interface (`uci.hpp`, `uci.cpp`, `uci_options.hpp`)**
- `UCIHandler::loop()`: Main UCI command processing loop
- UCI options: Hash size, Threads, MultiPV, Ponder, Contempt, etc.
- Time management with stability-based adjustments

**Zobrist Hashing (`zobrist.hpp`, `zobrist.cpp`)**
- Random keys for: piece-square combinations, castling rights, en passant files, side-to-move
- Incremental key updates during move making

**Tuning Infrastructure (`tuning.hpp`, `tuning.cpp`)**
- SPSA tuning parameter definitions
- Texel tuner (`tuner/texel_tuner.cpp`) for evaluation parameter optimization
- Tuner build target: `mingw32-make tuner`

**Data Generation (`datagen.hpp`, `datagen.cpp`)**
- Self-play game generation for NNUE training
- Binpack format support for compact storage
- Filter scripts: `kaggle_filter_data.py`

### Important Implementation Details

**Move Making Flow**
1. `do_move()` updates board state, incremental PST scores, Zobrist keys
2. `StateInfo` captures reversible state (for undo)
3. `set_check_info()` recalculates checkers, pinners, check squares
4. `undo_move()` restores previous state via `StateInfo`

**Search Techniques Implemented**
- Pruning: Null move, futility, reverse futility (static null move), razoring, SEE pruning, late move pruning, multi-cut, ProbCut, history-based pruning
- Reductions: Late move reductions (LMR) with dynamic adjustments, internal iterative reductions (IIR)
- Extensions: Check, singular, double, recapture, passed pawn (7th rank), capture (SEE-based), PV, mate threat, negative (for unexpected fail-highs)

**Key Data Structures Locations**
- `types.hpp`: Fundamental enums and types (Color, PieceType, Square, Rank, File, Direction, CastlingRights)
- `search_constants.hpp`: Search tuning parameters
- PST values are in `eval.hpp` as constexpr arrays
- Magic constants are computed at runtime in `magic.cpp`

**Test Organization**
Unit tests are in `tests/` directory:
- `test_main.cpp`: Test runner (includes other test files)
- `test_types.cpp`, `test_bitboard.cpp`, `test_move.cpp`, `test_board.cpp`, `test_movegen.cpp`
- See `UNIT_TESTS.md` for comprehensive test plan

## Development Workflow

### Making Changes
1. Make code changes in `include/` and `src/`
2. Build: `mingw32-make clean & mingw32-make all`
3. Verify correctness: `echo "perft 5" | .\output\main.exe`
4. Run benchmark: `echo "bench" | .\output\main.exe`
5. For unit tests: `mingw32-make run-tests`

### Performance Testing
1. Build baseline: `mingw32-make clean & mingw32-make pext` (save as `output\main-old.exe`)
2. Make changes and rebuild
3. Run SPRT test: `.\run_sprt.bat` (select appropriate time control)
4. Typical SPRT bounds: STC [0,10] Elo, LTC [0,5] Elo

### Tuning Evaluation Parameters
1. Generate training data or use existing EPD file
2. Build tuner: `mingw32-make tuner`
3. Run: `.\output\tuner.exe quiet-labeled.epd [max_positions] [iterations]`
4. Update parameter values in `tuning.hpp` based on results

## Common Pitfalls

### Bitboard Operations
- Always use proper shift functions (`shift<NORTH>()`, `shift<EAST>()`) to avoid edge wrapping issues
- Remember that `popcount()` returns int, not bitboard
- `pop_lsb(bb)` modifies the bitboard in place and returns the square

### Move Making/Unmaking
- Always pair `do_move()` with `undo_move()` - no exceptions
- `StateInfo` must outlive the move (typically stack-allocated in search)
- Null moves require separate `do_null_move()`/`undo_null_move()` calls

### UCI Protocol
- All UCI output must flush immediately (`std::cout.setf(std::ios::unitbuf)` in main)
- Bestmove must be sent even if search is stopped early
- Ponder move is optional second token after bestmove

### Threading
- All TT accesses must be thread-safe (currently uses atomic operations in TTEntry)
- History tables are per-thread to avoid contention
- Shared state (like stop flag) must be atomic

### Windows-Specific
- Use `mingw32-make` not `make` on Windows
- Path separators in Makefile: use `$(call FIXPATH,...)` for portability
- Static linking (`-static` in LFLAGS) avoids DLL dependencies

## File Organization
```
include/        - Header files (.hpp)
src/            - Implementation files (.cpp)
tests/          - Unit tests and EPD test suites
tuner/          - Texel tuning implementation
nnue_trainer/   - NNUE training scripts (Python)
data/           - Training data storage
output/         - Compiled binaries
cutechess/      - cutechess-cli for engine testing (not in repo)
```
