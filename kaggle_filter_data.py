# ============================================================================
# Kaggle Notebook: Filter NNUE Training Data (MULTI-THREADED + MONITORING)
# ============================================================================
#
# Purpose: Filter training data using ALL CPUs with REAL-TIME PROGRESS BAR
#
# Semua filter settings sudah di-hardcode sebagai default optimal di engine:
#   - skip in-check positions  ON
#   - qsearch stability margin 60 cp
#   - max score filter         2000 cp
#   - min pieces filter        <=5 pieces (skip trivial endgame)
#   - deduplication            ON  (~8 bytes/pos RAM)
#
# Gunakan token 'no_dedup' jika RAM terbatas (menonaktifkan deduplication).
# ============================================================================

import os
import subprocess
import multiprocessing
import time
import shutil
import glob
import sys
from pathlib import Path

# ============================================================================
# CONFIGURATION
# ============================================================================

INPUT_BINPACK = "/kaggle/input/data-train-nnue/training.binpack"
OUTPUT_DIR = "/kaggle/working"

# Engine repository
REPO_URL = "https://github.com/DhaniAAA/GC-Chess_Engine.git"
REPO_BRANCH = "main"

# ============================================================================
# INTERNAL CONFIG
# ============================================================================

ENGINE_DIR = f"{OUTPUT_DIR}/GC-Chess_Engine"
OUTPUT_BINPACK = f"{OUTPUT_DIR}/training_filtered.binpack"
ENTRY_SIZE = 40  # Pastikan ini sesuai dengan format data (biasanya 40 bytes)

def get_cpu_count():
    return multiprocessing.cpu_count()

def format_size(size_bytes):
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if size_bytes < 1024:
            return f"{size_bytes:.2f} {unit}"
        size_bytes /= 1024
    return f"{size_bytes:.2f} PB"

def format_time(seconds):
    if seconds < 60:
        return f"{seconds:.1f}s"
    elif seconds < 3600:
        return f"{int(seconds//60)}m {int(seconds%60)}s"
    else:
        return f"{int(seconds//3600)}h {int((seconds%3600)//60)}m"

def run_cmd(cmd, cwd=None, timeout=None, show_output=True):
    if show_output: print(f"$ {cmd}")
    try:
        result = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return result.returncode == 0, result.stdout, result.stderr
    except Exception as e:
        print(f"❌ Error: {e}")
        return False, "", str(e)

# ============================================================================
# MAIN SCRIPT
# ============================================================================

print("=" * 70)
print("   🔍 NNUE Training Data Filter (Multi-Threaded + Visual)")
print("=" * 70)

num_cpus = get_cpu_count()
print(f"🖥️  System: {num_cpus} CPUs available")

# --- Step 1: Verify Input ---
if not os.path.exists(INPUT_BINPACK):
    raise FileNotFoundError(f"Input not found: {INPUT_BINPACK}")

input_size = os.path.getsize(INPUT_BINPACK)
total_input_entries = input_size // ENTRY_SIZE
print(f"📁 Input:  {format_size(input_size)} ({total_input_entries:,} entries)")

# --- Step 2: Build Engine ---
print("\n🔨 Step 2: Build Engine")
if not os.path.exists(ENGINE_DIR):
    run_cmd(f"git clone --depth 1 -b {REPO_BRANCH} {REPO_URL} {ENGINE_DIR}")

if not os.path.exists(f"{ENGINE_DIR}/output/main"):
    run_cmd(f"make clean && make pext -j{num_cpus}", cwd=ENGINE_DIR, show_output=False)

engine_path = f"{ENGINE_DIR}/output/main"
if not os.path.exists(engine_path):
    if os.path.exists(f"{ENGINE_DIR}/main"): engine_path = f"{ENGINE_DIR}/main"
    else: raise FileNotFoundError("Engine binary not found")
print(f"✅ Engine built: {engine_path}")

# --- Step 3: Split Input File ---
print(f"\n✂️  Step 3: Splitting input into {num_cpus} parts...")

chunk_entries = total_input_entries // num_cpus
chunk_size = chunk_entries * ENTRY_SIZE
input_parts = []

if not glob.glob(f"{OUTPUT_DIR}/part_*.binpack"):
    with open(INPUT_BINPACK, 'rb') as f_in:
        for i in range(num_cpus):
            part_filename = f"{OUTPUT_DIR}/part_{i}.binpack"
            input_parts.append(part_filename)

            read_size = (input_size - (i * chunk_size)) if i == num_cpus - 1 else chunk_size
            print(f"   Writing part {i+1}/{num_cpus}: {format_size(read_size)}")

            with open(part_filename, 'wb') as f_out:
                bytes_written = 0
                block_sz = 1024 * 1024 * 100
                while bytes_written < read_size:
                    to_read = min(block_sz, read_size - bytes_written)
                    data = f_in.read(to_read)
                    if not data: break
                    f_out.write(data)
                    bytes_written += len(data)
else:
    print("   ⚠️ Parts already exist, skipping split...")
    input_parts = sorted(glob.glob(f"{OUTPUT_DIR}/part_*.binpack"))

# --- Step 4: Run Filter in Parallel with Monitor ---
print(f"\n🚀 Step 4: Running {num_cpus} filter processes in parallel...")

processes = []
output_parts = []

for i in range(num_cpus):
    inp_file = input_parts[i]
    out_file = f"{OUTPUT_DIR}/out_part_{i}.binpack"
    output_parts.append(out_file)
    # Hapus file output lama jika ada agar monitoring akurat
    if os.path.exists(out_file): os.remove(out_file)

    # Command: pakai defaults engine (qsearch=60, max_score=2000, min_pieces=5, dedup=ON)
    # Tidak perlu pass parameter — semua sudah optimal di C++ FilterConfig defaults.
    # Untuk disable dedup (RAM terbatas): tambahkan "no_dedup" di akhir command.
    cmd_str = f"datagen filter input {inp_file} output {out_file}\nquit\n"

    p = subprocess.Popen(
        [engine_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, # Kita tidak butuh stdout karena akan dimonitor lewat file
        stderr=subprocess.STDOUT,
        text=True,
        cwd=ENGINE_DIR
    )

    p.stdin.write(cmd_str)
    p.stdin.flush()
    processes.append(p)

print("   Processing started... (Watching output files)")
print("-" * 70)

# --- MONITORING LOOP ---
start_time = time.time()
last_check_time = start_time
last_size = 0

try:
    while True:
        # Check if processes are still alive
        all_done = all(p.poll() is not None for p in processes)

        # Calculate progress based on output file sizes
        current_total_size = sum(os.path.getsize(f) for f in output_parts if os.path.exists(f))
        current_entries = current_total_size // ENTRY_SIZE

        # Calculate stats
        now = time.time()
        elapsed = now - start_time
        delta_time = now - last_check_time

        if delta_time >= 1.0 or all_done: # Update every 1 second
            speed = (current_entries - last_size) / delta_time if delta_time > 0 else 0
            # Estimasi kasar progress berdasarkan asumsi retention rate 10-20%
            # (Sulit tahu 100% karena kita tidak tahu berapa banyak yg lolos filter)
            # Jadi kita tampilkan data mentah saja

            sys.stdout.write(
                f"\r⏱️  {format_time(elapsed)} | "
                f"Saved: {current_entries:,} pos | "
                f"Size: {format_size(current_total_size)} | "
                f"Speed: {int(speed):,} pos/sec  "
            )
            sys.stdout.flush()

            last_check_time = now
            last_size = current_entries

        if all_done:
            break

        time.sleep(0.5)

except KeyboardInterrupt:
    print("\n🛑 Interrupted by user!")
    for p in processes: p.terminate()

print("\n" + "-" * 70)

# Cleanup processes
for p in processes:
    p.stdout = None # Prevent read error
    p.communicate()

filter_time = time.time() - start_time
print(f"✅ All workers finished in {format_time(filter_time)}")

# --- Step 5: Merge Outputs ---
print(f"\n🔗 Step 5: Merging output files...")

final_output_entries = 0
with open(OUTPUT_BINPACK, 'wb') as f_final:
    for out_part in output_parts:
        if os.path.exists(out_part):
            size = os.path.getsize(out_part)
            final_output_entries += size // ENTRY_SIZE

            with open(out_part, 'rb') as f_part:
                shutil.copyfileobj(f_part, f_final)

            os.remove(out_part)

# Cleanup input parts
for p in input_parts:
    if os.path.exists(p): os.remove(p)

# --- Results ---
print("\n" + "=" * 50)
print(f"📊 FINAL STATISTICS")
print("=" * 50)
print(f"Input Entries:  {total_input_entries:,}")
print(f"Output Entries: {final_output_entries:,}")
print(f"Filtered Out:   {total_input_entries - final_output_entries:,}")
if total_input_entries > 0:
    print(f"Retention Rate: {(final_output_entries/total_input_entries*100):.1f}%")
    print(f"Avg Speed:      {int(total_input_entries/filter_time):,} input-entries/sec")
print(f"Output File:    {OUTPUT_BINPACK}")
print("=" * 50)