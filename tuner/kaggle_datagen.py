import os
import sys
import time
import json
import signal
import subprocess
import tarfile
import argparse
from pathlib import Path

# Pastikan output encoding aman di semua sistem operasi
if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

# ==============================================================================
# ⚙️ KONFIGURASI PENGATURAN (BISA DIUBAH SESUAI KEBUTUHAN)
# ==============================================================================
TOTAL_GAMES    = 50000        # Target total game self-play
BATCH_SIZE     = 1000         # Jumlah game per batch penyimpanan
NODES_PER_MOVE = 5000         # Batas node search per langkah (sesuai dokumen NNUE)
THREADS        = os.cpu_count() or 4  # Menggunakan seluruh 4 core vCPU Kaggle
MAX_HOURS      = 11.25        # Soft timeout (Kaggle max 12 jam)

# 🧠 PENGATURAN EVALUASI NNUE
USE_NNUE       = True         # True = Gunakan NNUE untuk evaluasi selfplay, False = Fallback HCE
NNUE_FILE      = "gc.nnue"    # Nama / path file bobot NNUE hasil training

# 🌟 FITUR PENGGABUNGAN FILE (MERGE)
AUTO_MERGE     = True         # True = Gabungkan seluruh chunk jadi 1 file tunggal
MERGED_NAME    = "dataset_all.txt" # Nama file hasil penggabungan
REMOVE_CHUNKS  = True         # True = Hapus chunk-chunk kecil setelah di-merge agar rapi

# Lokasi output folder dan path binary engine
OUTPUT_DIR     = Path("/kaggle/working/data")
POSSIBLE_ENGINE_PATHS = [
    Path("/kaggle/working/engine/output/main"),
    Path("/kaggle/working/GC-Chess_Engine/output/main"),
    Path("./output/main"),
    Path("./output/main.exe"),
    Path("./engine/output/main")
]
ENGINE_PATH = next((p for p in POSSIBLE_ENGINE_PATHS if p.exists()), POSSIBLE_ENGINE_PATHS[0])

# Deteksi otomatis letak file bobot NNUE (mendukung GCNNUE03 8-bucket dan GCNNUE02)
VALID_NNUE_FORMATS = {
    b"GCNNUE03": (803880, "GCNNUE03 (8 output buckets)"),
    b"GCNNUE02": (789516, "GCNNUE02 (single bucket)")
}

POSSIBLE_NNUE_PATHS = [
    Path("/kaggle/working/engine/output/gc.nnue"),
    Path("/kaggle/working/GC-Chess_Engine/output/gc.nnue"),
    Path("/kaggle/working/output/gc.nnue"),
    Path("/kaggle/working/engine/gc.nnue"),
    Path("/kaggle/working/GC-Chess_Engine/gc.nnue"),
    Path("/kaggle/working/gc.nnue"),
    Path("./output/gc.nnue"),
    Path("./gc.nnue"),
    Path(NNUE_FILE)
]

# Tambahkan pencarian otomatis di /kaggle/input jika user meng-upload dataset gc.nnue
if Path("/kaggle/input").exists():
    for p in sorted(Path("/kaggle/input").rglob("*.nnue"), reverse=True):
        POSSIBLE_NNUE_PATHS.insert(0, p)

def is_valid_nnue_file(path: Path) -> tuple[bool, str]:
    if not path.is_file():
        return False, f"File tidak ditemukan: {path}"
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            magic = f.read(8)
        if magic not in VALID_NNUE_FORMATS:
            valid_magics = [m.decode('ascii') for m in VALID_NNUE_FORMATS.keys()]
            return False, f"Magic header tidak dikenal di {path}: dapat {magic!r}, didukung {valid_magics}"
        expected_size, desc = VALID_NNUE_FORMATS[magic]
        if size != expected_size:
            return False, f"Ukuran file salah di {path}: dapat {size:,} bytes, seharusnya {expected_size:,} bytes ({desc})"
        return True, f"OK ({desc})"
    except Exception as e:
        return False, f"Gagal membaca {path}: {e}"

def find_best_nnue_path() -> Path:
    candidates = []
    for p in POSSIBLE_NNUE_PATHS:
        valid, desc = is_valid_nnue_file(p)
        if valid:
            candidates.append((p, desc))
    if not candidates:
        return next((p for p in POSSIBLE_NNUE_PATHS if p.is_file()), Path(NNUE_FILE))
    # Utamakan format terbaru GCNNUE03 jika ada
    v3_list = [p for p, desc in candidates if "GCNNUE03" in desc]
    if v3_list:
        return v3_list[0]
    return candidates[0][0]

RESOLVED_NNUE_PATH = find_best_nnue_path()

# Opsional: Hugging Face sync
HF_REPO        = None         # contoh: "username/gc-chess-data"
HF_TOKEN       = None         # contoh: "hf_xxxxxxxxxxxxxxxx"
# ==============================================================================


class KaggleDatagenManager:
    def __init__(self,
                 engine_path: Path = ENGINE_PATH,
                 total_games: int = TOTAL_GAMES,
                 batch_games: int = BATCH_SIZE,
                 threads: int = THREADS,
                 nodes: int = NODES_PER_MOVE,
                 output_dir: Path = OUTPUT_DIR,
                 max_runtime_hours: float = MAX_HOURS,
                 use_nnue: bool = USE_NNUE,
                 nnue_file: str = str(RESOLVED_NNUE_PATH),
                 auto_merge: bool = AUTO_MERGE,
                 merged_name: str = MERGED_NAME,
                 remove_chunks: bool = REMOVE_CHUNKS,
                 hf_repo: str = HF_REPO,
                 hf_token: str = HF_TOKEN):

        self.engine_path = Path(engine_path)
        self.total_games = total_games
        self.batch_games = batch_games
        self.threads = threads or (os.cpu_count() or 4)
        self.nodes = nodes
        self.output_dir = Path(output_dir)
        self.max_runtime_seconds = max_runtime_hours * 3600
        
        self.use_nnue = use_nnue
        self.nnue_file = nnue_file
        self.auto_merge = auto_merge
        self.merged_name = merged_name
        self.remove_chunks = remove_chunks
        self.hf_repo = hf_repo
        self.hf_token = hf_token
        
        if not self.hf_token:
            try:
                from kaggle_secrets import UserSecretsClient
                secrets = UserSecretsClient()
                self.hf_token = secrets.get_secret("HF_TOKEN")
                if not self.hf_repo:
                    self.hf_repo = secrets.get_secret("HF_REPO")
            except Exception:
                pass

        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.state_file = self.output_dir / "datagen_state.json"
        self.stop_requested = False
        self.start_time = time.time()
        
        signal.signal(signal.SIGINT, self._handle_signal)
        signal.signal(signal.SIGTERM, self._handle_signal)

    def _handle_signal(self, signum, frame):
        print(f"\n[INTERRUPT] Sinyal penghentian diterima! Menyelesaikan batch dengan aman...")
        self.stop_requested = True

    def load_state(self) -> dict:
        if self.state_file.exists():
            try:
                with open(self.state_file, "r") as f:
                    return json.load(f)
            except Exception as e:
                print(f"[WARN] Gagal membaca state file ({e}), membuat state baru.")
        return {
            "completed_games": 0,
            "total_positions": 0,
            "batches_completed": 0,
            "chunks": []
        }

    def save_state(self, state: dict):
        temp_file = self.state_file.with_suffix(".tmp")
        with open(temp_file, "w") as f:
            json.dump(state, f, indent=2)
        temp_file.replace(self.state_file)

    def upload_to_hf(self, file_path: Path):
        if not self.hf_repo or not self.hf_token:
            return
        try:
            from huggingface_hub import HfApi
            api = HfApi()
            api.upload_file(
                path_or_fileobj=str(file_path),
                path_in_repo=f"data/{file_path.name}",
                repo_id=self.hf_repo,
                repo_type="dataset",
                token=self.hf_token
            )
            print(f"[CLOUD BACKUP] Berhasil sync {file_path.name} ke HuggingFace: {self.hf_repo}")
        except Exception as e:
            print(f"[CLOUD BACKUP ERROR] Gagal sync ke HuggingFace: {e}")

    def run_batch(self, batch_index: int, games_to_run: int) -> tuple[int, Path]:
        chunk_file = self.output_dir / f"chunk_{batch_index:04d}.txt"
        
        # Susun perintah UCI termasuk konfigurasi NNUE
        uci_cmds = ["uci"]
        if self.use_nnue:
            uci_cmds.append("setoption name UseNNUE value true")
            if self.nnue_file:
                uci_cmds.append(f"setoption name EvalFile value {self.nnue_file}")
        else:
            uci_cmds.append("setoption name UseNNUE value false")
        uci_cmds.append("isready")
        uci_cmds.append(f"datagen games {games_to_run} threads {self.threads} nodes {self.nodes} out {chunk_file}")
        uci_cmds.append("quit\n")
        cmd = "\n".join(uci_cmds)

        print(f"\n" + "="*70)
        print(f"[BATCH {batch_index}] Memulai {games_to_run} games | {self.threads} threads | {self.nodes} nodes")
        print(f"[EVALUASI] {'NNUE (' + str(self.nnue_file) + ')' if self.use_nnue else 'HCE (Manual)'}")
        print(f"[TARGET SEMENTARA] {chunk_file.name}")
        print("="*70)
        
        batch_start = time.time()
        process = subprocess.Popen(
            [str(self.engine_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )

        positions_in_batch = 0
        nnue_verified = False
        try:
            process.stdin.write(cmd)
            process.stdin.flush()

            for line in process.stdout:
                line_str = line.strip()
                if not line_str:
                    continue

                if "NNUE loaded successfully" in line_str and not nnue_verified:
                    print(f"   [NNUE LOADED] {line_str}")
                    nnue_verified = True
                elif "info string NNUE" in line_str and "loaded successfully" not in line_str:
                    # Abaikan warning default 'gc.nnue' saat startup jika kita mengirim path EvalFile khusus yang valid
                    if "not found or unreadable: gc.nnue" in line_str and self.nnue_file and str(self.nnue_file) != "gc.nnue" and Path(self.nnue_file).is_file():
                        continue
                    print(f"   [NNUE ERROR/WARN] {line_str}")

                if "datagen starting:" in line_str and self.use_nnue and not nnue_verified:
                    print("   [NNUE CRITICAL WARNING] Datagen berjalan TANPA NNUE (fallback ke HCE)! Periksa file gc.nnue Anda!")

                if "completed:" in line_str:
                    print(f"   {line_str}")
                elif "datagen:" in line_str:
                    parts = line_str.split()
                    if len(parts) >= 2 and "/" in parts[1]:
                        done_g = int(parts[1].split("/")[0])
                        total_g = int(parts[1].split("/")[1])
                        # Hanya cetak jika kelipatan 100 game atau saat game terakhir
                        if done_g % 100 == 0 or done_g == total_g:
                            print(f"   {line_str}")
                    if "positions saved" in line_str:
                        parts = line_str.split()
                        for idx, p in enumerate(parts):
                            if p == "positions" and idx > 0 and parts[idx-1].isdigit():
                                positions_in_batch = int(parts[idx-1])

            process.wait()
        except Exception as e:
            print(f"[ERROR] Eksekusi batch gagal: {e}")
            if process.poll() is None:
                process.terminate()

        if positions_in_batch == 0 and chunk_file.exists():
            with open(chunk_file, "r") as f:
                positions_in_batch = sum(1 for _ in f)

        batch_duration = time.time() - batch_start
        rate = (positions_in_batch / batch_duration) if batch_duration > 0 else 0
        print(f"[BATCH {batch_index} SELESAI] +{positions_in_batch:,} posisi ({rate:.1f} pos/s)")

        return positions_in_batch, chunk_file

    def merge_all(self):
        """Menggabungkan semua chunk menjadi 1 file tunggal"""
        chunks = sorted(self.output_dir.glob("chunk_*.txt"))
        if not chunks:
            return None
        
        merged_path = self.output_dir / self.merged_name
        print(f"\n[MERGE] Menggabungkan {len(chunks)} file chunk ke dalam 1 file: {merged_path.name}...")
        
        total_lines = 0
        with open(merged_path, "w", encoding="utf-8") as outfile:
            for chunk in chunks:
                with open(chunk, "r", encoding="utf-8") as infile:
                    for line in infile:
                        outfile.write(line)
                        total_lines += 1
                if self.remove_chunks:
                    try:
                        chunk.unlink()
                    except Exception:
                        pass
        
        size_mb = merged_path.stat().st_size / (1024 * 1024)
        print(f"[MERGE SELESAI] Total {total_lines:,} posisi tergabung di {merged_path.name} ({size_mb:.2f} MB)")
        if self.remove_chunks:
            print(f"[CLEANUP] File chunk individual telah dihapus agar folder tetap rapi.")
        return merged_path

    def compress_all(self):
        tar_path = self.output_dir / "gc_nnue_dataset.tar.gz"
        print(f"\n[ARCHIVE] Mengompres dataset ke {tar_path.name}...")
        try:
            with tarfile.open(tar_path, "w:gz") as tar:
                merged_file = self.output_dir / self.merged_name
                if merged_file.exists():
                    tar.add(merged_file, arcname=merged_file.name)
                for chunk in sorted(self.output_dir.glob("chunk_*.txt")):
                    tar.add(chunk, arcname=chunk.name)
                if self.state_file.exists():
                    tar.add(self.state_file, arcname=self.state_file.name)
            size_mb = tar_path.stat().st_size / (1024 * 1024)
            print(f"[ARCHIVE SUCCESS] Berhasil membuat {tar_path.name} ({size_mb:.2f} MB)")
            print(f"[DOWNLOAD READY] Tersedia untuk diunduh di tab 'Data' -> 'Output' Kaggle.")
        except Exception as e:
            print(f"[ARCHIVE ERROR] Gagal mengompres: {e}")

    def start(self):
        state = self.load_state()
        print("="*65)
        print("          GC-CHESS-ENGINE MASSIVE DATAGEN WORKER          ")
        print("="*65)
        print(f"Target Games  : {self.total_games:,}")
        print(f"Batch Size    : {self.batch_games:,} games / batch")
        print(f"Threads CPU   : {self.threads} vCPU")
        print(f"Evaluasi Mode : {'NNUE (' + str(self.nnue_file) + ')' if self.use_nnue else 'HCE (Manual)'}")
        print(f"Auto-Merge    : {'Aktif (' + self.merged_name + ')' if self.auto_merge else 'Nonaktif'}")
        print(f"Time Guard    : Maksimal {self.max_runtime_seconds / 3600:.2f} Jam")
        print(f"Posisi Awal   : {state['total_positions']:,} dari {state['completed_games']:,} games")
        print("="*65)

        if not self.engine_path.exists():
            print(f"\n[FATAL] Binary engine tidak ditemukan di {self.engine_path}")
            print("Jalankan kompilasi 'make clean && make -j$(nproc)' terlebih dahulu.")
            return

        if self.use_nnue:
            valid_nnue, reason = is_valid_nnue_file(Path(self.nnue_file))
            if not valid_nnue:
                print(f"\n[FATAL NNUE] File NNUE tidak valid -> {reason}")
                print("Pastikan file gc.nnue versi GCNNUE03 (803,880 bytes) atau GCNNUE02 (789,516 bytes) tersedia di /kaggle/working/.")
                print("Jika ingin datagen memakai HCE (tanpa NNUE), jalankan dengan argumen --no-nnue.")
                return
            else:
                size = Path(self.nnue_file).stat().st_size
                print(f"[NNUE VERIFIED] File bobot valid: {self.nnue_file} ({size:,} bytes, {reason})")

        batch_idx = state["batches_completed"] + 1

        while state["completed_games"] < self.total_games:
            elapsed = time.time() - self.start_time
            remaining_time = self.max_runtime_seconds - elapsed

            # Hentikan sebelum batas 12 jam Kaggle
            if remaining_time < 900 or self.stop_requested:
                print(f"\n[TIME-GUARD AKTIF] Batas waktu operasi tercapai (Sisa: {remaining_time/60:.1f} menit).")
                print("Menghentikan generator dan menggabungkan data...")
                break

            remaining_games = self.total_games - state["completed_games"]
            games_this_batch = min(self.batch_games, remaining_games)

            pos_count, chunk_path = self.run_batch(batch_idx, games_this_batch)

            state["completed_games"] += games_this_batch
            state["total_positions"] += pos_count
            state["batches_completed"] = batch_idx
            state["chunks"].append(chunk_path.name)
            self.save_state(state)

            if self.hf_repo and self.hf_token:
                self.upload_to_hf(chunk_path)

            print(f"[TOTAL PROGRESS] {state['completed_games']:,}/{self.total_games:,} games "
                  f"({state['total_positions']:,} quiet positions) | Berjalan: {elapsed/3600:.2f} jam")

            batch_idx += 1

        # Gabungkan semua file jika auto_merge aktif
        if self.auto_merge:
            self.merge_all()

        # Kompresi arsip
        self.compress_all()
        print("\n[SELESAI] Seluruh data telah aman dan siap untuk training NNUE!")


# Jalankan Datagen di Sel Notebook atau CLI
def main():
    parser = argparse.ArgumentParser(description="Kaggle Datagen Runner for GC-Chess-Engine")
    parser.add_argument("--engine", type=str, default=str(ENGINE_PATH))
    parser.add_argument("--games", type=int, default=TOTAL_GAMES)
    parser.add_argument("--batch", type=int, default=BATCH_SIZE)
    parser.add_argument("--threads", type=int, default=THREADS)
    parser.add_argument("--nodes", type=int, default=NODES_PER_MOVE)
    parser.add_argument("--out", type=str, default=str(OUTPUT_DIR))
    parser.add_argument("--max-hours", type=float, default=MAX_HOURS)
    parser.add_argument("--nnue", type=str, default=str(RESOLVED_NNUE_PATH), help="Path ke file bobot NNUE (misal gc.nnue)")
    parser.add_argument("--no-nnue", action="store_true", help="Gunakan HCE manual bukannya NNUE")
    parser.add_argument("--no-merge", action="store_true", help="Jangan gabungkan chunk jadi 1 file")
    parser.add_argument("--keep-chunks", action="store_true", help="Jangan hapus file chunk setelah di-merge")
    parser.add_argument("--hf-repo", type=str, default=HF_REPO)
    parser.add_argument("--hf-token", type=str, default=HF_TOKEN)

    # parse_known_args agar aman dijalankan langsung di sel Kaggle / Jupyter
    args, _ = parser.parse_known_args()

    runner = KaggleDatagenManager(
        engine_path=Path(args.engine),
        total_games=args.games,
        batch_games=args.batch,
        threads=args.threads,
        nodes=args.nodes,
        output_dir=Path(args.out),
        max_runtime_hours=args.max_hours,
        use_nnue=not args.no_nnue,
        nnue_file=args.nnue,
        auto_merge=not args.no_merge,
        remove_chunks=not args.keep_chunks,
        hf_repo=args.hf_repo,
        hf_token=args.hf_token
    )
    runner.start()

if __name__ == "__main__":
    main()
