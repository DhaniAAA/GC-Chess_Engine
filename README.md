# GC-Engine - A UCI Chess Engine

<p align="center">
  <strong>A powerful chess engine written in C++ with support for the Universal Chess Interface (UCI) protocol.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Version-1.2.1-blue" alt="Version">
  <img src="https://img.shields.io/badge/Language-C++17-orange" alt="Language">
  <img src="https://img.shields.io/badge/License-Open%20Source-green" alt="License">
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey" alt="Platform">
</p>

---

## 📋 Table of Contents

- [Features](#-features)
- [Building](#-building)
- [Usage](#-usage)
- [Testing](#-testing)
- [Project Structure](#-project-structure)
- [Acknowledgments](#-acknowledgments)
- [License](#-license)
- [Author](#-author)

---

## ✨ Features

### Search

- **Iterative Deepening** with aspiration windows
- **Principal Variation Search (PVS)** / Alpha-Beta pruning
- **Quiescence Search** with quiet check generation
- **Transposition Table** with multi-bucket hashing
- **Multi-Threading** support (Lazy SMP)
- **Move Ordering**:
  - Hash move / TT moves
  - Captures ordered by MVV-LVA and SEE
  - Killer moves (2 per ply)
  - Counter move heuristic
  - History heuristic
  - Continuation history (1-ply and 2-ply)

### Pruning & Reductions

- **Null Move Pruning** with verification search
- **Late Move Reductions (LMR)** with dynamic adjustments
- **Late Move Pruning (LMP)**
- **Futility Pruning** with depth-based margins
- **Reverse Futility Pruning** (Static Null Move)
- **Razoring** with depth-scaling margins
- **SEE Pruning** for captures and quiet moves
- **Delta Pruning** in quiescence search
- **Multi-Cut Pruning**
- **ProbCut**
- **Internal Iterative Reductions (IIR)**
- **History-Based Pruning** (Leaf, Counter, Followup)

### Extensions

- **Check Extension**
- **In-Check Extension**
- **Singular Extension** with multi-cut and double extensions
- **Recapture Extension**
- **Passed Pawn Extension** (7th rank)
- **Capture Extension** (SEE-based)
- **PV Extension**
- **Mate Threat Extension**
- **Negative Extension** for unexpected fail-highs

### Evaluation

- **Tapered Evaluation** (Middlegame/Endgame interpolation with non-linear phase scaling)
- **Material & Piece-Square Tables** (incrementally updated)
- **Pawn Structure**:
  - Passed pawns with connected bonus
  - Candidate passed pawns
  - Isolated, doubled, backward pawns
  - Phalanx and connected pawns
  - Pawn duo bonus
  - King proximity to passed pawns (endgame)
  - Blockaded passed pawn penalty
  - Rook behind passed pawn bonus
  - Rule of the Square bonus
- **Piece Activity**:
  - Mobility for all pieces (contextual evaluation)
  - Knight outposts
  - Knight tropism (distance to enemy king)
  - Bishop pair bonus
  - Bad bishop detection
  - Connected rooks bonus
  - Rook on open/semi-open files
  - Rook on 7th rank
- **King Safety**:
  - Attack units table with weighted attackers
  - Pawn shield evaluation
  - King ring / inner king ring attacks
  - Open/semi-open file penalties
- **Threats Evaluation**:
  - Piece threats from pawns
  - Minor piece and rook threats
  - Hanging pieces detection
- **Space Evaluation**
- **Tempo Bonus**
- **Lazy Evaluation** for speedup in won/lost positions
- **Pawn Hash Table** for efficient pawn structure caching

### Other Features

- **UCI Protocol** full support
- **Opening Book** support (Polyglot format)
- **Syzygy Tablebase** support (via Fathom)
- **Multi-PV** analysis mode
- **Pondering** with ponderhit support
- **Advanced Time Management** with stability-based adjustment and panic mode
- **Contempt** and **Dynamic Contempt** options
- **Tunable eval parameters** exposed as UCI options (see `src/tuning.cpp`); offline tuning scripts in `tuner/` (`kaggle_texel_tuner.py` + `quiet-labeled.epd`)
- **Built-in Profiler** for performance analysis (`mingw32-make internal-profile`, then run `bench`)
- **Benchmark** command for reproducible testing

---

## 🔧 Building

### Requirements

- **C++17** compatible compiler:
  - GCC 10+ (MinGW-w64 x86_64 on Windows; required for PEXT/BMI2 builds)
  - Clang 12+
  - MSVC 2019+ (not tested)
- **GNU Make** (or MinGW Make on Windows)

### Quick Build

#### 🪟 Windows (MinGW)

```bash
# Standard release build
mingw32-make

# Clean and rebuild
mingw32-make clean; mingw32-make all

# Build and run
mingw32-make run
```

#### 🐧 Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update && sudo apt-get install -y g++ make

# Standard release build
make

# Clean and rebuild
make clean && make all
```

### Advanced Build Options (Windows)

```bash
# Debug build (no optimization, with symbols)
mingw32-make debug

# Build with PEXT/BMI2 support (Intel Haswell+, AMD Zen 3+)
mingw32-make pext

# Profile-Guided Optimization (PGO) - Step 1: Generate profile
mingw32-make pgo-generate
# Run benchmarks (engine reads commands from stdin):
echo "bench" | .\output\main.exe
# Step 2: Use profile data
mingw32-make pgo-use

# PEXT + PGO for maximum performance
mingw32-make pext-pgo-generate
# Run benchmarks:
echo "bench" | .\output\main.exe
mingw32-make pext-pgo-use

# Internal profiling build (for performance analysis)
mingw32-make internal-profile
```

### Build Output

| Platform | Executable Location |
| -------- | ------------------- |
| Windows  | `output/main.exe`   |
| Linux    | `output/main`       |

---

## 🎮 Usage

### UCI Mode

Run the engine and it will automatically start in UCI mode:

```bash
./output/main.exe
```

### UCI Commands

| Command                                  | Description                                      |
| ---------------------------------------- | ------------------------------------------------ |
| `uci`                                    | Display engine info and available options        |
| `isready`                                | Check if engine is ready                         |
| `ucinewgame`                             | Start a new game (clears hash table and history) |
| `position [fen \| startpos] [moves ...]` | Set position                                     |
| `go [options]`                           | Start search                                     |
| `stop`                                   | Stop current search                              |
| `ponderhit`                              | Opponent played the expected ponder move         |
| `quit`                                   | Exit engine                                      |

### Go Command Options

| Option        | Description                   |
| ------------- | ----------------------------- |
| `depth N`     | Search to depth N             |
| `nodes N`     | Search N nodes                |
| `movetime N`  | Search for N milliseconds     |
| `wtime N`     | White time remaining (ms)     |
| `btime N`     | Black time remaining (ms)     |
| `winc N`      | White increment per move (ms) |
| `binc N`      | Black increment per move (ms) |
| `movestogo N` | Moves until next time control |
| `infinite`    | Search until stopped          |
| `ponder`      | Start pondering               |

### Non-Standard Commands

| Command                          | Description                                            |
| -------------------------------- | ------------------------------------------------------ |
| `perft N`                        | Run perft test to depth N                              |
| `divide N`                       | Run divided perft to depth N                           |
| `d`                              | Display current board position                         |
| `eval`                           | Show static evaluation of current position             |
| `bench [depth] [threads] [hash]` | Run benchmark (default: depth 13, 1 thread, 16MB hash) |

---

## 🧪 Testing

### Quick Tests

```bash
# Test perft (move generation correctness)
echo "perft 5" | ./output/main.exe

# Test search
echo "go depth 10" | ./output/main.exe

# Run built-in benchmark
echo "bench" | ./output/main.exe

# Run benchmark with custom settings
echo "bench 15 4 256" | ./output/main.exe  # depth 15, 4 threads, 256MB hash
```

### Perft Results (Verified)

| Position | Depth | Nodes       | Time  |
| -------- | ----- | ----------- | ----- |
| Starting | 5     | 4,865,609   | ~1.3s |
| Starting | 6     | 119,060,324 | ~74s  |
| Kiwipete | 4     | 4,085,603   | ~0.4s |

### Test Suites

The engine includes test positions in the `tests/` directory:

| File               | Description             |
| ------------------ | ----------------------- |
| `wac.epd`          | Win at Chess positions  |
| `bratko_kopec.epd` | Bratko-Kopec test suite |

Run the WAC suite over a UCI pipe (requires `python-chess`):

```bash
python tests/wac_run.py [depth] [max_pos]
```

### Gauntlet Testing

Use the provided batch scripts for automated testing against other engines:

```bash
# Run gauntlet against multiple engines
./run_gauntlet.bat

# Run SPRT testing
./run_sprt.bat
```

---

## 📁 Project Structure

```text
src/            Flat engine sources (15 × .cpp); compiled via wildcard src/*.cpp
include/        Flat headers (21 × .hpp)
tests/          wac.epd, bratko_kopec.epd, wac_run.py (UCI-pipe runner), baseline.txt
tuner/          Offline tuning data/scripts (kaggle_texel_tuner.py, quiet-labeled.epd)
book/           Polyglot opening book (Perfect2023.bin)
output/         Build output (output/main.exe) — gitignored
engines/        Opponent engines for gauntlet/SPRT — gitignored
cutechess/      Cutechess CLI binaries for gauntlet/SPRT — gitignored
```

Entrypoint: `src/main.cpp` → `init_engine()` (`Position::init()` covers
Bitboards/Magics/Zobrist) → `UCI::UCIHandler::loop()`.

> Note: `tests/baseline.txt` still references the removed `wac`/`bk`/`datagen`
> engine commands and deleted test scripts — treat its thresholds as historical,
> not runnable.

---

## 🙏 Acknowledgments

- [Stockfish](https://stockfishchess.org/) - Inspiration for many search techniques and algorithms
- [Chess Programming Wiki](https://www.chessprogramming.org/) - Invaluable resource for chess programming
- [Fathom](https://github.com/jdart1/Fathom) - Syzygy tablebase probing library
- [Cutechess](https://github.com/cutechess/cutechess) - Chess engine testing tool

---

## 📄 License

This project is open source. Feel free to use, modify, and distribute.

---

## 👨‍💻 Author

**Dhani** - GC-Engine Development

---

<p align="center">
  <em>Happy chess programming!</em> ♟️
</p>
