import os
import subprocess
import multiprocessing
import time
import sys
import re
from pathlib import Path

# ============================================================================
# CONFIGURATION - EDIT THESE VALUES
# ============================================================================

# --- Datagen Settings ---
# ┌─────────────────────────────────────────────────────────────────────┐
# │  PARAMETER GUIDE (untuk dataset NNUE berkualitas)                  │
# │                                                                     │
# │  Target output: ≥100M posisi (5M+ games × ~20 pos/game)            │
# │  Rumus: parameter_model(10.5M) × 10 = 100M posisi minimum          │
# └─────────────────────────────────────────────────────────────────────┘

GAMES = 10000000              # 10M games → ~200M posisi (cukup untuk HalfKP 256)
                              # Jika punya waktu lebih, 20M lebih baik
THREADS = 0                   # 0 = auto-detect semua CPU (Kaggle: 4 atau 224)
HASH_MB = 2048                # 2GB hash table — cukup untuk 224 threads
                              # Lebih besar = lebih sedikit hash collision = search lebih akurat

DEPTH = 8                     # Search depth per posisi
                              # 7  = cepat tapi noisy (~2× speed vs depth 8)
                              # 8  = sweet-spot untuk engine <3000 Elo ✅
                              # 9+ = lebih akurat tapi SANGAT lambat

SOFT_NODES = 0                # soft_nodes=0 → search SELALU mencapai depth yang diminta
                              # Trade-off: lebih lambat tapi label JAUH lebih akurat

NODES = 0                     # Hard node limit (0 = unlimited, pakai depth saja)

RESIGN = 2500                 # Adjudicate win jika |score| >= 2500cp selama 4 ply berturut
                              # 2500cp = ~queen advantage, hampir pasti menang
                              # Range baik: 2000-3000

QSEARCH = 100                  # Buang posisi jika |static_eval - qsearch| > 60cp
                              # = filter posisi taktis tidak stabil
                              # 40-80 = range baik. Lebih rendah = lebih ketat

SEARCH_MARGIN = 70            # Buang posisi jika |static_eval - search_score| > 70cp
                              # = filter posisi dengan faktor tersembunyi
                              # 50-100 = range baik

MULTIPV = 2                   # Random Multi-PV: pilih acak dari top-N moves saat random phase
                              # 1  = pure random (semua legal moves, lebih banyak blunder)
                              # 2  = top-2 berdasarkan static eval ✅ (default, lebih sane)
                              # 3+ = lebih beragam, cocok jika pakai book depth pendek

# --- Score Mixing (Lambda-weighted WDL) ---
# Fitur modern: gabungkan search eval dengan game result
#   SCORE_LAMBDA = 1.0 → pure search score (tidak ada mixing, backward compatible)
#   SCORE_LAMBDA = 0.5 → 50% eval + 50% game result ← DIREKOMENDASIKAN untuk NNUE
#   SCORE_LAMBDA = 0.0 → pure game result
SCORE_LAMBDA = 0.75
WDL_SCALE = 400               # Skala sigmoid untuk konversi cp → WDL probability
                              # sigmoid(cp / WDL_SCALE): 400cp ≈ 73% win probability
                              # 300-500 = range wajar. Jangan ubah kecuali tahu artinya.

RULE50_DECAY = True           # Scale eval dengan (100-rule50)/100 sebelum mixing
                              # True  = posisi mendekati 50-move draw dianggap netral ✅
                              # False = eval tetap meski rule50 counter tinggi

OUTPUT_FILENAME = "training.binpack"

# --- Auto-Resume Settings ---
RESUME_ON_INCOMPLETE = True  # Auto-resume if datagen stops early
MAX_RESUME_ATTEMPTS = 100     # Maximum resume attempts
MIN_PROGRESS_PER_RUN = 1000  # Minimum games per run (to detect stuck)
KEEP_ALIVE_INTERVAL = 50     # Print keep-alive every N seconds (prevents idle timeout)

# --- Engine Repository ---
REPO_URL = "https://github.com/DhaniAAA/GC-Chess_Engine.git"
REPO_BRANCH = "main"        # Branch to clone

# --- Build Settings ---
USE_PEXT = True             # Use PEXT/BMI2 instructions (faster on modern CPUs)
PGO_BUILD = False           # Use Profile-Guided Optimization (slower build, faster run)

# ============================================================================
# DO NOT EDIT BELOW THIS LINE
# ============================================================================

OUTPUT_DIR = "/kaggle/working"
ENGINE_DIR = f"{OUTPUT_DIR}/GC-Chess_Engine"
OUTPUT_PATH = f"{OUTPUT_DIR}/{OUTPUT_FILENAME}"

def get_cpu_count():
    """Get available CPU count"""
    return multiprocessing.cpu_count()

def run_cmd(cmd, cwd=None, timeout=None, show_output=True):
    """Execute shell command"""
    if show_output:
        print(f"$ {cmd}")

    try:
        result = subprocess.run(
            cmd,
            shell=True,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        if show_output:
            if result.stdout.strip():
                lines = result.stdout.strip().split('\n')
                if len(lines) > 20:
                    print('\n'.join(lines[:10]))
                    print(f"... ({len(lines) - 20} lines omitted) ...")
                    print('\n'.join(lines[-10:]))
                else:
                    print(result.stdout)
            if result.stderr.strip():
                stderr_lines = [l for l in result.stderr.split('\n')
                               if 'warning' not in l.lower() and l.strip()]
                if stderr_lines:
                    print(f"[stderr] {stderr_lines[:5]}")
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        print(f"⏰ Timeout after {timeout}s")
        return False, "", "Timeout"
    except Exception as e:
        print(f"❌ Error: {e}")
        return False, "", str(e)

def format_size(size_bytes):
    """Format bytes to human readable"""
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if size_bytes < 1024:
            return f"{size_bytes:.2f} {unit}"
        size_bytes /= 1024
    return f"{size_bytes:.2f} PB"

def format_time(seconds):
    """Format seconds to human readable"""
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = int(seconds % 60)
    if hours > 0:
        return f"{hours}h {minutes}m {secs}s"
    elif minutes > 0:
        return f"{minutes}m {secs}s"
    else:
        return f"{secs}s"

def format_number(n):
    """Format large numbers with suffix"""
    if n >= 1_000_000:
        return f"{n/1_000_000:.2f}M"
    elif n >= 1_000:
        return f"{n/1_000:.1f}K"
    return str(n)

def count_existing_positions(filepath):
    """Count positions in existing binpack file"""
    if not os.path.exists(filepath):
        return 0
    size = os.path.getsize(filepath)
    return size // 40  # 40 bytes per entry

def estimate_games_from_positions(positions):
    """Estimate games from positions (avg ~18 pos/game)"""
    return positions // 18

# ============================================================================
# MAIN SCRIPT
# ============================================================================

print("=" * 70)
print("   🎯 GC-Chess Engine - NNUE Training Data Generator")
print("   For Kaggle Notebooks (with Auto-Resume)")
print("=" * 70)
print()

# System info
num_cpus = get_cpu_count()
threads = THREADS if THREADS > 0 else num_cpus

print(f"🖥️  System Information:")
print(f"   CPUs available: {num_cpus}")
print(f"   Threads to use: {threads}")
print()

print(f"📋 Configuration:")
print(f"   Games:        {format_number(GAMES)}")
print(f"   Depth:        {DEPTH}")
print(f"   Nodes:        {format_number(NODES) if NODES > 0 else 'unlimited'}")
print(f"   Hash:         {HASH_MB} MB")
print(f"   MultiPV:      {MULTIPV} {'(pure random)' if MULTIPV <= 1 else f'(top-{MULTIPV} eval-scored)'}")
print(f"   Score lambda: {SCORE_LAMBDA} {'[WDL MIXING AKTIF]' if SCORE_LAMBDA < 1.0 else '(pure search score)'}")
if SCORE_LAMBDA < 1.0:
    print(f"   WDL scale:    {WDL_SCALE} cp")
print(f"   Rule50 decay: {'enabled' if RULE50_DECAY else 'disabled'}")
print(f"   QSEARCH:      {QSEARCH} cp")
print(f"   Search margin:{SEARCH_MARGIN} cp")
print(f"   Output:       {OUTPUT_PATH}")
print(f"   Resume:       {'Enabled' if RESUME_ON_INCOMPLETE else 'Disabled'}")
print()

# ======================================
# Step 1: Clone Repository
# ======================================
print("📥 Step 1: Clone Repository")
print("-" * 50)

if os.path.exists(ENGINE_DIR):
    print(f"📂 Engine directory exists, pulling latest changes...")
    success, _, _ = run_cmd(f"cd {ENGINE_DIR} && git fetch && git reset --hard origin/{REPO_BRANCH}", timeout=60)
    if not success:
        print("⚠️ Git pull failed, removing and re-cloning...")
        run_cmd(f"rm -rf {ENGINE_DIR}")
        os.makedirs(ENGINE_DIR, exist_ok=True)

if not os.path.exists(ENGINE_DIR) or not os.path.exists(f"{ENGINE_DIR}/Makefile"):
    print(f"📥 Cloning repository...")
    print(f"   URL: {REPO_URL}")
    print(f"   Branch: {REPO_BRANCH}")
    success, _, _ = run_cmd(
        f"git clone --depth 1 -b {REPO_BRANCH} {REPO_URL} {ENGINE_DIR}",
        timeout=120
    )
    if not success:
        raise RuntimeError("Failed to clone repository")

print("✅ Repository ready!")
print()

# ======================================
# Step 2: Build Engine
# ======================================
print("🔨 Step 2: Build Chess Engine")
print("-" * 50)

build_start = time.time()

print("🧹 Cleaning previous build...")
run_cmd("make clean", cwd=ENGINE_DIR, timeout=60, show_output=False)

build_type = "PEXT" if USE_PEXT else "standard"
print(f"🔧 Building engine ({build_type} mode, {num_cpus} cores)...")

if USE_PEXT:
    success, stdout, stderr = run_cmd(f"make pext -j{num_cpus}", cwd=ENGINE_DIR, timeout=600, show_output=False)
    if not success:
        print("⚠️ PEXT build failed, trying standard build...")
        if stderr.strip():
            # Tampilkan error compiler (hanya 30 baris terakhir)
            err_lines = [l for l in stderr.strip().split('\n') if l.strip()]
            for line in err_lines[-30:]:
                print(f"   {line}")
        run_cmd("make clean", cwd=ENGINE_DIR, timeout=60, show_output=False)
        success, stdout, stderr = run_cmd(f"make all -j{num_cpus}", cwd=ENGINE_DIR, timeout=600, show_output=False)
else:
    success, stdout, stderr = run_cmd(f"make all -j{num_cpus}", cwd=ENGINE_DIR, timeout=600, show_output=False)

if not success:
    print("❌ Build failed! Compiler errors:")
    if stderr.strip():
        err_lines = stderr.strip().split('\n')
        for line in err_lines[-50:]:
            print(f"   {line}")
    if stdout.strip():
        out_lines = stdout.strip().split('\n')
        for line in out_lines[-20:]:
            print(f"   {line}")
    raise RuntimeError("Failed to build engine! Check compiler errors above.")

build_time = time.time() - build_start
print(f"✅ Build complete in {format_time(build_time)}")

engine_path = f"{ENGINE_DIR}/output/main"
if not os.path.exists(engine_path):
    for alt in [f"{ENGINE_DIR}/main", f"{ENGINE_DIR}/gc-engine"]:
        if os.path.exists(alt):
            engine_path = alt
            break
    else:
        print("❌ Engine binary not found!")
        run_cmd(f"ls -la {ENGINE_DIR}/output/ 2>/dev/null || ls -la {ENGINE_DIR}/")
        raise FileNotFoundError("Engine binary not found")

run_cmd(f"chmod +x {engine_path}", show_output=False)
print(f"   Binary: {engine_path}")
print()

# ======================================
# Step 3: Run Data Generation with Auto-Resume
# ======================================
print("🎮 Step 3: Generate Training Data")
print("-" * 50)

import threading

def run_single_datagen(games_to_generate, output_file, timeout_seconds=None):
    """
    Run a single datagen session.
    Returns (games_completed, positions_saved, timed_out)
    """
    datagen_cmd = (
        f"datagen start"
        f" games {games_to_generate}"
        f" threads {threads}"
        f" hash {HASH_MB}"
        f" depth {DEPTH}"
        f" resign {RESIGN}"
        f" qsearch {QSEARCH}"
        f" search_margin {SEARCH_MARGIN}"
        f" multipv {MULTIPV}"
        f" lambda {SCORE_LAMBDA}"
        f" wdl_scale {WDL_SCALE}"
        f"{ ' rule50_decay' if RULE50_DECAY else ''}"
        f" softnodes {SOFT_NODES}"
    )
    if NODES > 0:
        datagen_cmd += f" nodes {NODES}"
    datagen_cmd += f" output {output_file}"

    print(f"\n🚀 Running: {datagen_cmd}")

    # Start process
    process = subprocess.Popen(
        [engine_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=ENGINE_DIR
    )

    # Send command
    process.stdin.write(datagen_cmd + "\n")
    process.stdin.flush()

    # Track progress
    progress_pattern = re.compile(
        r'Progress:\s*(\d+)/(\d+)\s*games\s*\|\s*(\d+)\s*positions'
    )

    last_games = 0
    last_positions = 0
    last_progress_time = time.time()
    start_time = time.time()
    completed = False
    timed_out = False

    def read_output():
        nonlocal last_games, last_positions, last_progress_time, completed

        while True:
            try:
                line = process.stdout.readline()
                if not line:
                    if process.poll() is not None:
                        break
                    continue

                text = line.strip()
                if not text:
                    continue

                # Skip noise
                if text.startswith("info ") and "string" not in text:
                    continue
                if text.startswith("bestmove"):
                    continue

                # Progress line
                match = progress_pattern.search(text)
                if match:
                    games = int(match.group(1))
                    total_games = int(match.group(2))
                    positions = int(match.group(3))

                    last_games = games
                    last_positions = positions
                    last_progress_time = time.time()

                    # Progress bar
                    pct = (games / total_games * 100) if total_games > 0 else 0
                    bar_width = 30
                    filled = int(bar_width * games / total_games) if total_games > 0 else 0
                    bar = "█" * filled + "░" * (bar_width - filled)

                    elapsed = time.time() - start_time
                    progress_str = f"\r[{bar}] {pct:5.1f}% | Games: {format_number(games)}/{format_number(total_games)} | Pos: {format_number(positions)} | Time: {format_time(elapsed)}"
                    sys.stdout.write(progress_str.ljust(120))
                    sys.stdout.flush()
                    continue

                # Completion markers
                if "=== Data Generation Complete ===" in text or "=== Data Generation Statistics ===" in text:
                    print(f"\n{text}")
                    completed = True
                    continue

                # Print other important lines
                if "===" in text or "Threads" in text or "Games target" in text:
                    print(text)

            except Exception as e:
                break

    # Start reader thread
    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()

    # Wait with timeout and stuck detection
    stuck_timeout = 300  # 2 minutes without progress = stuck
    last_keepalive = time.time()

    try:
        while True:
            reader.join(timeout=5)  # Check every 5 seconds
            if not reader.is_alive():
                break

            current_time = time.time()
            elapsed = current_time - start_time

            # Keep-alive heartbeat (prevents Kaggle idle timeout)
            if current_time - last_keepalive >= KEEP_ALIVE_INTERVAL:
                last_keepalive = current_time
                # Check file size as checkpoint indicator
                file_size = 0
                if os.path.exists(output_file):
                    file_size = os.path.getsize(output_file)
                print(f"\n💓 [{format_time(elapsed)}] Heartbeat: {format_number(last_games)} games, {format_size(file_size)} saved")

            # Check if stuck
            time_since_progress = current_time - last_progress_time
            if time_since_progress > stuck_timeout and last_games > 0:
                print(f"\n⚠️ No progress for {int(time_since_progress)}s - stopping this session")
                timed_out = True
                break

            # Check overall timeout
            if timeout_seconds and elapsed > timeout_seconds:
                print(f"\n⏰ Timeout after {format_time(elapsed)}")
                timed_out = True
                break

    except KeyboardInterrupt:
        print("\n⚠️ Interrupted!")
        timed_out = True

    # Cleanup process
    if process.poll() is None:
        try:
            process.stdin.write("quit\n")
            process.stdin.flush()
            process.wait(timeout=5)
        except:
            process.terminate()
            try:
                process.wait(timeout=2)
            except:
                process.kill()

    print()
    return last_games, last_positions, not timed_out and completed

# ======================================
# Main loop with auto-resume
# ======================================

total_start_time = time.time()
total_games_generated = 0
total_positions_saved = 0
attempt = 0

# Check existing progress
existing_positions = count_existing_positions(OUTPUT_PATH)
if existing_positions > 0:
    existing_games = estimate_games_from_positions(existing_positions)
    print(f"📂 Found existing data: ~{format_number(existing_games)} games ({format_number(existing_positions)} positions)")
    total_games_generated = existing_games
    total_positions_saved = existing_positions

while total_games_generated < GAMES and attempt < MAX_RESUME_ATTEMPTS:
    attempt += 1
    games_remaining = GAMES - total_games_generated

    print(f"\n{'='*50}")
    print(f"🔄 Attempt {attempt}/{MAX_RESUME_ATTEMPTS}")
    print(f"   Target: {format_number(games_remaining)} more games")
    print(f"   Total progress: {format_number(total_games_generated)}/{format_number(GAMES)}")
    print(f"{'='*50}")

    # Use a temp file for this session, then append
    if attempt == 1 and existing_positions == 0:
        # First run, write directly
        session_output = OUTPUT_PATH
    else:
        # Subsequent runs, use temp file
        session_output = f"{OUTPUT_PATH}.session{attempt}"

    games_made, positions_made, completed = run_single_datagen(
        games_remaining,
        session_output,
        timeout_seconds=3600 * 6  # 6 hour max per session
    )

    # If using temp file, append to main file
    if session_output != OUTPUT_PATH and os.path.exists(session_output):
        session_size = os.path.getsize(session_output)
        if session_size > 0:
            print(f"📎 Appending {format_size(session_size)} to main file...")
            run_cmd(f"cat {session_output} >> {OUTPUT_PATH}", show_output=False)
        run_cmd(f"rm -f {session_output}", show_output=False)

    # Update totals from file
    new_total_positions = count_existing_positions(OUTPUT_PATH)
    positions_added = new_total_positions - total_positions_saved
    games_added = estimate_games_from_positions(positions_added)

    total_positions_saved = new_total_positions
    total_games_generated = estimate_games_from_positions(total_positions_saved)

    print(f"\n📊 Session result: +{format_number(games_added)} games, +{format_number(positions_added)} positions")
    print(f"📊 Total: {format_number(total_games_generated)} games, {format_number(total_positions_saved)} positions")

    # Check if we made enough progress
    if games_added < MIN_PROGRESS_PER_RUN and not completed:
        print(f"⚠️ Session made too little progress ({games_added} < {MIN_PROGRESS_PER_RUN})")
        if not RESUME_ON_INCOMPLETE:
            break

    if completed or total_games_generated >= GAMES * 0.99:
        print("✅ Target reached!")
        break

    if not RESUME_ON_INCOMPLETE:
        break

    print(f"🔄 Auto-resuming in 5 seconds...")
    time.sleep(5)

total_time = time.time() - total_start_time

# ======================================
# Step 4: Final Results
# ======================================
print()
print("📊 Step 4: Final Results")
print("-" * 50)

if os.path.exists(OUTPUT_PATH):
    output_size = os.path.getsize(OUTPUT_PATH)
    num_entries = output_size // 40

    print(f"✅ Data generation complete!")
    print()
    print(f"📈 Statistics:")
    print(f"   ┌─────────────────────────────────────────────┐")
    print(f"   │ Games (estimated): {format_number(total_games_generated):>18} │")
    print(f"   │ Positions saved:   {format_number(num_entries):>18} │")
    print(f"   │ Avg pos/game:      {num_entries // max(total_games_generated, 1):>18} │")
    print(f"   │ Resume attempts:   {attempt:>18} │")
    print(f"   └─────────────────────────────────────────────┘")
    print()
    print(f"📁 Output File:")
    print(f"   Path: {OUTPUT_PATH}")
    print(f"   Size: {format_size(output_size)}")
    print()
    print(f"⏱️  Performance:")
    print(f"   Total time: {format_time(total_time)}")
    if total_time > 0 and total_games_generated > 0:
        print(f"   Avg speed:  {total_games_generated / total_time:.1f} games/sec")
        print(f"   Avg speed:  {num_entries / total_time:.0f} positions/sec")
    print()

    completion_pct = total_games_generated / GAMES * 100
    if completion_pct >= 99:
        print("=" * 70)
        print("✅ DONE! Download the training data from Kaggle's 'Output' tab")
        print("=" * 70)
    else:
        print("=" * 70)
        print(f"⚠️  Completed: {completion_pct:.1f}% ({format_number(total_games_generated)}/{format_number(GAMES)} games)")
        print("   Data was saved - you can still use it!")
        print("=" * 70)
else:
    print("❌ Output file was not created!")
    print(f"   Expected: {OUTPUT_PATH}")
    print()
    print("💡 Try running with fewer threads or check for errors above.")
