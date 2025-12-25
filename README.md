# GC-Engine - A UCI Chess Engine

A powerful chess engine written in C++ with support for the Universal Chess Interface (UCI) protocol.

## Features

### Search

- **Iterative Deepening** with aspiration windows
- **Principal Variation Search (PVS)** / Alpha-Beta pruning
- **Quiescence Search** with quiet check generation
- **Transposition Table** with multi-bucket hashing
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
- **Futility Pruning**
- **Reverse Futility Pruning** (Static Null Move)
- **Razoring**
- **SEE Pruning** for captures and quiet moves
- **Delta Pruning** in quiescence search
- **Multi-Cut Pruning**
- **ProbCut**
- **Internal Iterative Reductions (IIR)**

### Extensions

- **Check Extension**
- **In-Check Extension**
- **Singular Extension** with multi-cut
- **Recapture Extension**
- **Passed Pawn Extension** (7th rank)
- **Negative Extension** for unexpected fail-highs

### Evaluation

- **Tapered Evaluation** (Middlegame/Endgame interpolation)
- **Material & Piece-Square Tables** (incrementally updated)
- **Pawn Structure**:
  - Passed pawns with connected bonus
  - Isolated, doubled, backward pawns
  - Phalanx and connected pawns
  - King proximity to passed pawns
  - Blockaded passed pawn penalty
  - Rook behind passed pawn bonus
- **Piece Activity**:
  - Mobility for all pieces
  - Knight outposts
  - Knight tropism (distance to enemy king)
  - Bishop pair bonus
  - Bad bishop detection
  - Connected rooks bonus
  - Rook on open/semi-open files
  - Rook on 7th rank
- **King Safety**:
  - Attack units table
  - Pawn shield evaluation
  - Open/semi-open file penalties
- **Space Evaluation**
- **Tempo Bonus**
- **Lazy Evaluation** for speedup in won/lost positions
- **Pawn Hash Table**

### Other Features

- **UCI Protocol** support
- **Opening Book** support (Polyglot format)
- **Syzygy Tablebase** support (via Fathom)
- **Multi-PV** analysis mode
- **Pondering**
- **Time Management** with panic mode

## Building

### Requirements

- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Make (GNU Make or MinGW Make on Windows)

### Compile

```bash
# Release build
make

# Debug build
make DEBUG=yes

# Clean
make clean
```

### Build Output

The compiled executable will be in `output/main.exe` (Windows) or `output/main` (Linux/macOS).

## Usage

### UCI Mode

Run the engine and it will automatically start in UCI mode:

```bash
./output/main.exe
```

### UCI Commands

```
uci              - Display engine info and options
isready          - Check if engine is ready
ucinewgame       - Start a new game
position [fen | startpos] [moves ...]  - Set position
go [depth N | movetime N | wtime/btime/winc/binc] - Start search
stop             - Stop current search
quit             - Exit engine
```

### Testing Commands

```
perft N          - Run perft test to depth N
divide N         - Run divided perft to depth N
d                - Display current board
eval             - Show static evaluation
bench            - Run benchmark
```

### UCI Options

| Option       | Type   | Default  | Description                    |
| ------------ | ------ | -------- | ------------------------------ |
| Hash         | spin   | 256      | Transposition table size (MB)  |
| Threads      | spin   | 1        | Number of search threads       |
| MultiPV      | spin   | 1        | Number of principal variations |
| Ponder       | check  | false    | Enable pondering               |
| MoveOverhead | spin   | 30       | Move overhead (ms)             |
| OwnBook      | check  | false    | Use opening book               |
| BookPath     | string | book.bin | Path to opening book           |
| SyzygyPath   | string |          | Path to Syzygy tablebases      |
| KingSafety   | spin   | 83       | King safety weight (%)         |

## Perft Results

Verified correct move generation:

| Position | Depth | Nodes       | Time  |
| -------- | ----- | ----------- | ----- |
| Starting | 5     | 4,865,609   | ~0.5s |
| Starting | 6     | 119,060,324 | ~12s  |
| Kiwipete | 4     | 4,085,603   | ~0.8s |

## Project Structure

```
chess_engine/
├── include/           # Header files
│   ├── board.hpp      # Board representation
│   ├── bitboard.hpp   # Bitboard operations
│   ├── move.hpp       # Move encoding
│   ├── movegen.hpp    # Move generation
│   ├── eval.hpp       # Evaluation constants
│   ├── search.hpp     # Search algorithm
│   └── ...
├── src/               # Source files
│   ├── main.cpp       # Entry point
│   ├── uci.cpp        # UCI protocol
│   ├── board.cpp      # Board implementation
│   ├── movegen.cpp    # Move generation
│   ├── search.cpp     # Search implementation
│   ├── eval.cpp       # Evaluation function
│   └── ...
├── output/            # Compiled binaries
├── tests/             # Test files
└── makefile           # Build configuration
```

## Testing

### Quick Test

```bash
# Test perft (move generation correctness)
echo "perft 5" | ./output/main.exe

# Test search
echo "go depth 10" | ./output/main.exe
```

### Gauntlet Testing

Use the provided batch scripts for automated testing:

```bash
./run_gauntlet.bat    # Test against multiple engines
```

## Acknowledgments

- [Stockfish](https://stockfishchess.org/) - Inspiration for many search techniques
- [Chess Programming Wiki](https://www.chessprogramming.org/) - Invaluable resource
- [Fathom](https://github.com/jdart1/Fathom) - Syzygy tablebase probing

## License

This project is open source. Feel free to use, modify, and distribute.

## Author

GC-Engine Development Team

---

_Happy chess programming!_ ♟️
