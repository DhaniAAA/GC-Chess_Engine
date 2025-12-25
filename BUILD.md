# GC-Engine Build Guide

Panduan lengkap untuk membangun GC-Engine dengan berbagai konfigurasi optimasi.

---

## 📋 Prerequisites

### Compiler Requirements

| Compiler  | Versi Minimum | Catatan                                 |
| --------- | ------------- | --------------------------------------- |
| MinGW-w64 | GCC 10+       | **Rekomendasi: x86_64 untuk PEXT/BMI2** |
| MSVC      | VS 2019+      | Belum ditest                            |

### Cek Versi Compiler

```bash
g++ --version
```

## 🔨 Build Commands

### Build Standar (Rekomendasi Umum)

```bash
mingw32-make clean; mingw32-make all
```

---

### Build dengan PEXT/BMI2 (Rekomendasi untuk CPU Modern)

```bash
mingw32-make clean; mingw32-make pext
```

---

### Build dengan PGO (Profile-Guided Optimization)

**Step 1: Generate Profile**

```bash
mingw32-make clean; mingw32-make pgo-generate
```

**Step 2: Run Benchmarks** (untuk mengumpulkan data profil)

```bash
# Jalankan perft
echo "perft 6" | .\output\main.exe

# Atau benchmark search
echo "go depth 15" | .\output\main.exe
```

**Step 3: Build dengan Profile**

```bash
mingw32-make pgo-use
```

---

### Build PEXT + PGO (Performa Maksimal)

**Step 1: Generate Profile dengan PEXT**

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

### Build Debug (untuk Development)

```bash
mingw32-make clean
mingw32-make debug
```

- Tanpa optimasi (`-O0`)
- Dengan debug symbols (`-g`)
- Untuk debugging dengan GDB

---

### Build Tuner and Run

```bash
# Default (1428000 posisi, 100 iterasi)
output\tuner.exe tuner\quiet-labeled.epd

# Build
mingw32-make tuner
```

---

## 📊 Perbandingan Build

| Build Type   | Command           | Kecepatan | Kompatibilitas             | Notes                 |
| ------------ | ----------------- | --------- | -------------------------- | --------------------- |
| **Standar**  | `make all`        | Baseline  | Semua CPU                  | Default               |
| **PEXT**     | `make pext`       | +5-15%    | Intel Haswell+, AMD Zen 3+ | Butuh compiler 64-bit |
| **PGO**      | `make pgo-*`      | +10-15%   | Semua CPU                  | 2 langkah             |
| **PEXT+PGO** | `make pext-pgo-*` | +15-25%   | Intel Haswell+, AMD Zen 3+ | Performa maksimal     |
| **Debug**    | `make debug`      | Lambat    | Semua CPU                  | Untuk development     |

---

## 🚀 Quick Start

**Untuk CPU Modern (Intel gen 4+ / AMD Zen 3+):**

```bash
mingw32-make clean
mingw32-make pext
echo "perft 5" | .\output\main.exe
```

**Untuk CPU Lama atau Kompatibilitas Maksimal:**

```bash
mingw32-make clean
mingw32-make all
echo "perft 5" | .\output\main.exe
```

---

_Last updated: 2025-12-25_
