# Panduan Proyek: GC-Chess-Engine

## 🛠 Perintah Penting

- **Build:** `mingw32-make pext` atau `mingw32-make all`

## 🔍 Protokol Audit & Optimasi (Engine Focus)

### Fokus Audit:

1. **Move Ordering Efficiency:** Identifikasi apakah move ordering (MVV-LVA, Killer Moves, History Heuristics) bekerja optimal. Analisis mengapa node count per depth terlalu tinggi.
2. **Selective Search Logic:** Periksa implementasi LMR (Late Move Reductions), Null Move Pruning, dan Futility Pruning. Cari logika yang terlalu konservatif atau terlalu agresif.
3. **Evaluation Bottlenecks:** Cari redundansi dalam perhitungan evaluasi (misal: king safety atau pawn structure yang dihitung berulang kali tanpa caching).
4. **Memory & TT:** Audit implementasi Transposition Table (TT) untuk potensi collision atau pemborosan memory.

### Standar Performa:

- Setiap optimasi harus berorientasi pada peningkatan **NPS (Nodes Per Second)** atau pengurangan **EBF (Effective Branching Factor)**.
- Prioritaskan perbaikan pada fungsi `Search()`, `Evaluate()`, dan `Quiescence()`.
