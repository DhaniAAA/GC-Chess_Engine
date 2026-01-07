# GC-Engine Build Guide

A complete guide to building a GC-Engine with various optimization configurations.

---

## 📋 Prerequisites

### Compiler Requirements

#### Windows

| Compiler  | Versi Minimum | Catatan                                 |
| --------- | ------------- | --------------------------------------- |
| MinGW-w64 | GCC 10+       | **Rekomendasi: x86_64 untuk PEXT/BMI2** |
| MSVC      | VS 2019+      | Not tested                              |

#### Linux

| Compiler | Versi Minimum | Catatan                                |
| -------- | ------------- | -------------------------------------- |
| GCC      | 10+           | **Rekomendasi untuk performa terbaik** |
| Clang    | 12+           | Alternative                            |

### Check Compiler Version

```bash
# Windows (MinGW)
g++ --version

# Linux / macOS
g++ --version
# atau
clang++ --version
```

## 🔨 Build Commands

### 🪟 Windows

#### Build Standar (Recommended)

```bash
mingw32-make clean & mingw32-make all
```

#### Build dengan PEXT/BMI2 (Recommended for CPU Modern)

```bash
mingw32-make clean & mingw32-make pext
```

---

### 🐧 Linux

#### Build Standar

```bash
make clean && make all
```

#### Build with PEXT/BMI2 (CPU Modern)

```bash
make clean && make pext
```

#### Using Clang (alternative)

```bash
make CXX=clang++ clean && make CXX=clang++ all
```

---

### Build with PGO (Profile-Guided Optimization)

**Step 1: Generate Profile**

```bash
mingw32-make clean; mingw32-make pgo-generate
```

**Step 2: Run Benchmarks** (for profile data collection)

```bash
# Run perft
echo "perft 6" | .\output\main.exe

# Using Bench for CMD uci
echo "bench" | .\output\main.exe

# Or benchmark search
echo "go depth 15" | .\output\main.exe

```

**Step 3: Build with Profile**

```bash
mingw32-make pgo-use
```

---

### Build PEXT + PGO (Performance Maximum)

**Step 1: Generate Profile with PEXT**

```bash
mingw32-make clean; mingw32-make pext-pgo-generate
```

**Step 2: Run Benchmarks**

```bash
echo "perft 6" | .\output\main.exe
```

**Step 3: Build Final**

```bash
mingw32-make pext-pgo-use
```

---

### Build Debug (for Development)

```bash
mingw32-make clean
mingw32-make debug
```

- Without optimization (`-O0`)
- With debug symbols (`-g`)
- For debugging with GDB

---

### Build Tuner and Run

```bash
# Default (1428000 posisi, 100 iterasi)
output\tuner.exe tuner\quiet-labeled.epd

# Build
mingw32-make tuner
```

---

## 📊 Build Comparison

| Build Type   | Command           | Speed    | Compatibility              | Notes                 |
| ------------ | ----------------- | -------- | -------------------------- | --------------------- |
| **Standar**  | `make all`        | Baseline | All CPU                    | Default               |
| **PEXT**     | `make pext`       | +5-15%   | Intel Haswell+, AMD Zen 3+ | Butuh compiler 64-bit |
| **PGO**      | `make pgo-*`      | +10-15%  | All CPU                    | 2 langkah             |
| **PEXT+PGO** | `make pext-pgo-*` | +15-25%  | Intel Haswell+, AMD Zen 3+ | Performa maksimal     |
| **Debug**    | `make debug`      | Slow     | All CPU                    | Untuk development     |

---

## 🚀 Quick Start

### 🪟 Windows

**For CPU Modern (Intel gen 4+ / AMD Zen 3+):**

```bash
mingw32-make clean & mingw32-make pext
echo "perft 5" | .\output\main.exe
```

**For CPU Old or Maximum Compatibility:**

```bash
mingw32-make clean & mingw32-make all
echo "perft 5" | .\output\main.exe
```

### 🐧 Linux

**Install dependencies (Ubuntu/Debian):**

```bash
sudo apt-get update
sudo apt-get install -y g++ make
```

**Build and run:**

```bash
make clean && make all
# Or CPU Modern
make clean && make pext

echo "perft 5" | ./output/main
```

---

## 🔧 Troubleshooting

### Linux: "error: undefined reference to `pthread_create`"

Make sure `-pthread` flag is in Makefile. Update to latest version.

### Linux: `aligned_alloc` not found

Make sure using GCC 10+ or Clang 12+ with C++17 support.

```bash
make clean && make all
```

### Windows: 'mingw32-make' is not recognized

Make sure MinGW-w64 is installed and added to PATH.

---

_Last updated: 2026-01-07_
