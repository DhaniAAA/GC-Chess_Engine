# Changelog

All notable changes to GC-Engine will be documented in this file.

---

## [1.2] - 2026-01-06

### Highlights

**GC-Engine v1.2** is a release that focuses on improving the search power and stability of the engine with various pruning and extension technique optimizations.

### ✨ Added

#### Search Improvements

- **Enhanced LMR (Late Move Reductions)** - Formula Stockfish-style with dynamic adjustments
- **Improved Singular Extensions** - Double extensions dan multi-cut integration
- **History-Based Pruning** - Leaf, Counter, dan Followup history pruning
- **Better Move Ordering** - Continuation history (1-ply dan 2-ply)
- **Multi-Cut Pruning** - Depth 12+ with configurable parameters
- **ProbCut** - Probabilistic cutoffs for deep searches

#### Evaluation Enhancements

- **Threats Evaluation** - Detection of threats from pawns, minor pieces, and rooks
- **Advanced King Safety** - King ring attacks with weighted attackers
- **Improved Pawn Evaluation** - Candidate passed pawns, pawn duo bonus, Rule of the Square
- **Minor Piece Coordination** - Evaluation of knight and bishop coordination

#### New Features

- **Dynamic Contempt** - Contempt adjustment based on position
- **Built-in Profiler** - Performance analysis with `bench` command
- **SPSA Tuning Support** - Parameter tuning via UCI options
- **Texel Tuner** - Evaluation parameter tuning from EPD positions
- **Multi-threading** - Lazy SMP support up to 128 threads

#### Build System

- Profile-Guided Optimization (PGO) build support
- PEXT/BMI2 support for modern CPU (Intel Haswell+, AMD Zen 3+)
- Internal profiling build option
- Improved Makefile with multiple build targets

### ⚡ Changed

- Optimized search constants for balance between speed and tactical accuracy
- Improved time management with stability-based adjustment
- Better aspiration window handling
- Enhanced pondering with ponderhit support

### 🔧 UCI Options Baru

| Option           | Type  | Default | Description                       |
| ---------------- | ----- | ------- | --------------------------------- |
| Contempt         | spin  | 20      | Contempt value (-100 to 100)      |
| Dynamic Contempt | check | true    | Adjust contempt based on position |
| Table Memory     | spin  | 64      | Additional table memory (MB)      |
| PawnValueMG/EG   | spin  | -       | Tunable pawn values               |
| KnightValueMG/EG | spin  | -       | Tunable knight values             |
| BishopValueMG/EG | spin  | -       | Tunable bishop values             |
| RookValueMG/EG   | spin  | -       | Tunable rook values               |
| QueenValueMG/EG  | spin  | -       | Tunable queen values              |
| KingSafetyWeight | spin  | 83      | King safety weight (%)            |

### 📦 Build Commands

```bash
mingw32-make              # Standard release build
mingw32-make pext         # With BMI2/PEXT support
mingw32-make pgo-generate # PGO Step 1: Generate profile
mingw32-make pgo-use      # PGO Step 2: Use profile
mingw32-make debug        # Debug build
mingw32-make tuner        # Build Texel tuner
```

---

## [1.1] - Previous Release

- Initial public release
- Basic UCI protocol support
- Alpha-Beta search with PVS
- Transposition table
- Basic evaluation function

---

## [1.0] - Initial Release

- Core engine functionality
- Move generation
- Basic search algorithm
