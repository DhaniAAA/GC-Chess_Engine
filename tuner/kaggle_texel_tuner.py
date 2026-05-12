#!/usr/bin/env python3
"""
=============================================================================
GC-Chess Engine - Texel Tuner for Kaggle Notebook
=============================================================================
Builds the C++ Texel Tuner from GitHub and runs it with all available CPUs.

HOW TO USE ON KAGGLE:
  1. Create a new Kaggle Notebook (Python)
  2. Set Accelerator to "None" (CPU only, more cores available)
  3. Copy-paste ALL of this script into a single Code cell
  4. Run the cell

WHAT IT DOES:
  1. Clones the GC-Chess_Engine repo from GitHub
  2. Compiles the C++ Texel Tuner with -O3 -march=native
  3. Downloads/uses the quiet-labeled.epd dataset (~725K positions)
  4. Runs Texel Tuning v9 with ~320 parameters + score normalization
  5. Outputs final tuned values ready to paste into tuning.cpp

ESTIMATED TIME:
  - Compilation: ~30 seconds
  - Loading 725K positions: ~10 seconds
  - Tuning (100 iterations): ~2-6 hours depending on CPU count

TIPS FOR FASTER TUNING:
  - Reduce MAX_POSITIONS to 200000 for faster iterations
  - Reduce ITERATIONS to 50 for a quick test
  - Use all available Kaggle hours (12h session limit)

PARAMETER CATEGORIES (~320 total):
  - Material Values (10)
  - Piece Activity Bonuses (10)
  - Pawn Structure (28)
  - Passed Pawn Bonuses (49)
  - Mobility Tables with monotonicity (132)
  - King Safety weights + PawnShield (14)
  - Piece Activity Details (14)
  - Material Imbalance (10)
  - King Safety Extended: SafeCheck, Tropism, ShieldQuality, StormDanger (54)
  - Piece Patterns: Fianchetto, TrappedKnight (4)
=============================================================================
"""

import subprocess
import os
import sys
import time
import multiprocessing

# =============================================================================
# CONFIGURATION - Adjust these as needed
# =============================================================================
GITHUB_REPO    = "https://github.com/DhaniAAA/GC-Chess_Engine.git"
BRANCH         = "main"
MAX_POSITIONS  = 725000      # Number of positions to use (725K = full dataset)
ITERATIONS     = 100         # Tuning iterations (100 is good, 50 for quick test)
MANUAL_K       = 0           # 0 = auto-find optimal K, otherwise set manually (e.g. 1.13)
THREADS        = 0           # 0 = auto-detect (use all CPUs), or set specific number (e.g. 4)
WORK_DIR       = "/kaggle/working"
# =============================================================================


def run_cmd(cmd, cwd=None, check=True, capture=False):
    """Run a shell command and print output in real-time."""
    print(f"\n{'='*60}")
    print(f"  CMD: {cmd}")
    print(f"{'='*60}")

    if capture:
        result = subprocess.run(
            cmd, shell=True, cwd=cwd, check=check,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True
        )
        print(result.stdout)
        return result.stdout
    else:
        process = subprocess.Popen(
            cmd, shell=True, cwd=cwd,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1
        )
        output_lines = []
        for line in process.stdout:
            print(line, end="", flush=True)
            output_lines.append(line)
        process.wait()

        if check and process.returncode != 0:
            raise subprocess.CalledProcessError(process.returncode, cmd)

        return "".join(output_lines)


def main():
    start_time = time.time()

    cpu_count = multiprocessing.cpu_count()
    print(f"""
╔══════════════════════════════════════════════════════════╗
║       GC-Chess Engine - Texel Tuner v9 (Kaggle)        ║
║       ~320 Params + Score Normalization                 ║
╠══════════════════════════════════════════════════════════╣
║  Repo:       {GITHUB_REPO:<43s}║
║  Branch:     {BRANCH:<43s}║
║  Positions:  {MAX_POSITIONS:<43,d}║
║  Iterations: {ITERATIONS:<43d}║
║  Threads:    {(THREADS if THREADS > 0 else cpu_count):<43d}║
║  CPU Cores:  {cpu_count:<43d}║
╚══════════════════════════════════════════════════════════╝
""")

    repo_dir = os.path.join(WORK_DIR, "GC-Chess_Engine")

    # =========================================================================
    # STEP 1: Clone Repository
    # =========================================================================
    print("\n🔻 STEP 1: Cloning repository...")

    if os.path.exists(repo_dir):
        print(f"  Repository already exists at {repo_dir}, pulling latest...")
        run_cmd(f"git pull origin {BRANCH}", cwd=repo_dir)
    else:
        run_cmd(f"git clone --depth 1 --branch {BRANCH} {GITHUB_REPO} {repo_dir}")

    # =========================================================================
    # STEP 2: Check EPD dataset
    # =========================================================================
    print("\n📦 STEP 2: Checking EPD dataset...")

    epd_path = os.path.join(repo_dir, "tuner", "quiet-labeled.epd")

    if os.path.exists(epd_path):
        file_size = os.path.getsize(epd_path)
        line_count = sum(1 for _ in open(epd_path))
        print(f"  ✔ Found: {epd_path}")
        print(f"    Size: {file_size / 1024 / 1024:.1f} MB")
        print(f"    Lines: {line_count:,}")
    else:
        # Try downloading from a known source
        print("  ⚠ EPD file not found in repo. Attempting download...")
        epd_urls = [
            "https://raw.githubusercontent.com/DhaniAAA/GC-Chess_Engine/main/tuner/quiet-labeled.epd",
        ]

        downloaded = False
        for url in epd_urls:
            try:
                print(f"  Trying: {url}")
                run_cmd(f'wget -q "{url}" -O "{epd_path}"', check=False)
                if os.path.exists(epd_path) and os.path.getsize(epd_path) > 1000:
                    downloaded = True
                    break
            except Exception:
                continue

        if not downloaded:
            print("  ❌ Could not download EPD file!")
            print("  Please upload quiet-labeled.epd to /kaggle/input/ or the repo.")

            # Check kaggle input
            kaggle_epd = "/kaggle/input/quiet-labeled.epd"
            if os.path.exists(kaggle_epd):
                epd_path = kaggle_epd
                print(f"  ✔ Found at Kaggle input: {epd_path}")
            else:
                sys.exit(1)

    # =========================================================================
    # STEP 3: Compile Texel Tuner
    # =========================================================================
    print("\n🔨 STEP 3: Compiling Texel Tuner...")

    # Install build dependencies (Kaggle usually has g++ but just in case)
    run_cmd("apt-get update -qq && apt-get install -y -qq g++ make > /dev/null 2>&1 || true")

    # Check g++ version
    run_cmd("g++ --version | head -1", cwd=repo_dir, capture=True)

    # Clean and build
    run_cmd("make tuner-clean || true", cwd=repo_dir, check=False)

    compile_cmd = (
        f"make tuner -j{cpu_count} "
        f'CXX=g++ '
        f'CXXFLAGS="-std=c++17 -O3 -march=native -flto -funroll-loops -DNDEBUG -pthread"'
    )
    run_cmd(compile_cmd, cwd=repo_dir)

    # Find the tuner executable
    tuner_exe = None
    candidates = [
        os.path.join(repo_dir, "output", "tuner"),
        os.path.join(repo_dir, "output", "tuner.exe"),
        os.path.join(repo_dir, "tuner"),
    ]
    for c in candidates:
        if os.path.exists(c):
            tuner_exe = c
            break

    if not tuner_exe:
        print("  ❌ Tuner executable not found after compilation!")
        run_cmd(f"find {repo_dir}/output -type f 2>/dev/null || ls -la {repo_dir}/output/",
                cwd=repo_dir, check=False)
        sys.exit(1)

    print(f"  ✔ Tuner built: {tuner_exe}")

    # Make it executable (Linux)
    os.chmod(tuner_exe, 0o755)

    # =========================================================================
    # STEP 4: Run Texel Tuning
    # =========================================================================
    print(f"\n🚀 STEP 4: Running Texel Tuning...")
    threads_to_use = THREADS if THREADS > 0 else cpu_count
    print(f"   Executable:  {tuner_exe}")
    print(f"   EPD File:    {epd_path}")
    print(f"   Positions:   {MAX_POSITIONS:,}")
    print(f"   Iterations:  {ITERATIONS}")
    print(f"   Threads:     {threads_to_use} {'(auto)' if THREADS == 0 else '(manual)'}")
    print(f"   Manual K:    {'auto' if MANUAL_K == 0 else MANUAL_K}")
    print(f"\n{'─'*60}")

    # Build command
    k_arg = MANUAL_K if MANUAL_K > 0 else 0
    tuner_cmd = f"{tuner_exe} {epd_path} {MAX_POSITIONS} {ITERATIONS} {k_arg} {threads_to_use}"

    # Run and capture output
    results_file = os.path.join(WORK_DIR, "tuning_results.txt")

    process = subprocess.Popen(
        tuner_cmd, shell=True, cwd=repo_dir,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1
    )

    all_output = []
    final_values_started = False
    final_values = []

    for line in process.stdout:
        print(line, end="", flush=True)
        all_output.append(line)

        # Capture final values section
        if "FINAL TUNED VALUES" in line:
            final_values_started = True
        if final_values_started:
            final_values.append(line)

    process.wait()

    # Save full output
    with open(results_file, "w") as f:
        f.writelines(all_output)

    # =========================================================================
    # STEP 5: Summary
    # =========================================================================
    elapsed = time.time() - start_time
    hours = int(elapsed // 3600)
    minutes = int((elapsed % 3600) // 60)
    seconds = int(elapsed % 60)

    print(f"\n\n{'═'*60}")
    print(f"  TUNING COMPLETE!")
    print(f"  Total Time: {hours}h {minutes}m {seconds}s")
    print(f"  Results saved to: {results_file}")
    print(f"{'═'*60}")

    if final_values:
        print("\n📋 FINAL TUNED VALUES (copy to tuning.cpp):")
        print("─" * 60)
        for line in final_values:
            print(line, end="")
    else:
        # Try to extract from results file
        print("\n📋 Extracting final values from output...")
        capture = False
        with open(results_file, "r") as f:
            for line in f:
                if "FINAL TUNED VALUES" in line:
                    capture = True
                if capture:
                    print(line, end="")

    print(f"\n\n{'═'*60}")
    print(f"  HOW TO APPLY:")
    print(f"  1. Copy the 'FINAL TUNED VALUES' section above")
    print(f"  2. Paste into src/tuning.cpp (replace existing values)")
    print(f"  3. Rebuild the engine: make clean && make")
    print(f"  4. Run WAC test: echo 'wac quiet' | ./output/main")
    print(f"{'═'*60}")


if __name__ == "__main__":
    main()
