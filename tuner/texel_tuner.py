#!/usr/bin/env python3
"""
Texel Tuner Wrapper for GitHub Actions & Local Use
==================================================
This script builds and runs the high-performance C++ Texel Tuner.

Usage:
    python tuner/texel_tuner.py [epd_file] [max_positions] [iterations]

Example:
    python tuner/texel_tuner.py tuner/quiet-labeled.epd 500000 100
"""

import sys
import os
import subprocess
import platform
import argparse
import shutil

def main():
    parser = argparse.ArgumentParser(description="Build and run GC-Chess Texel Tuner (C++)")
    parser.add_argument("epd_file", nargs="?", default="tuner/quiet-labeled.epd", help="Path to EPD file")
    parser.add_argument("max_positions", nargs="?", default="500000", help="Max positions to load")
    parser.add_argument("iterations", nargs="?", default="100", help="Tuning iterations")
    parser.add_argument("--skip-build", action="store_true", help="Skip compilation step")

    args = parser.parse_args()

    # Detect OS
    is_windows = platform.system() == "Windows"
    exe_name = "tuner.exe" if is_windows else "tuner_bin"
    output_dir = "output"

    # Ensure output directory exists
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    exe_path = os.path.join(output_dir, exe_name)

    # 1. Compilation Step
    if not args.skip_build:
        print(f"[{'Windows' if is_windows else 'Linux'}] Compiling Texel Tuner...")

        build_cmd = []
        if is_windows:
            # Try mingw32-make first, then make
            make_cmd = "mingw32-make" if shutil.which("mingw32-make") else "make"
            build_cmd = [make_cmd, "tuner"]
        else:
            build_cmd = ["make", "tuner"]

        try:
            subprocess.check_call(build_cmd, cwd=".")
            print("✔ Compilation successful.")
        except subprocess.CalledProcessError as e:
            print(f"❌ Compilation failed with exit code {e.returncode}")
            sys.exit(1)
        except FileNotFoundError:
            print("❌ 'make' or 'mingw32-make' not found. Please install build tools (MinGW or build-essential).")
            sys.exit(1)

    if not os.path.exists(exe_path):
        # Fallback location check if makefile puts it elsewhere
        if os.path.exists(exe_name):
             shutil.move(exe_name, exe_path)
        elif os.path.exists(f"tuner/{exe_name}"):
             shutil.move(f"tuner/{exe_name}", exe_path)
        else:
             # Just assume the makefile output logic puts it in output/tuner.exe or similar
             # On current makefile it seems to go to root or tuner/
             pass

    # Check again
    possible_paths = [
        exe_path,
        f"output/{exe_name}",
        f"tuner/{exe_name}",
        exe_name
    ]

    final_exe = None
    for p in possible_paths:
        if os.path.exists(p):
            final_exe = p
            break

    if not final_exe:
        print(f"❌ Could not find compiled executable: {exe_name}")
        sys.exit(1)

    # 2. Execution Step
    print(f"\n🚀 Running Texel Tuner: {final_exe}")
    print(f"   EPD File:   {args.epd_file}")
    print(f"   Positions:  {args.max_positions}")
    print(f"   Iterations: {args.iterations}")
    print("-" * 60)

    run_cmd = [final_exe, args.epd_file, str(args.max_positions), str(args.iterations)]

    try:
        # Run and stream output
        process = subprocess.Popen(
            run_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            bufsize=1
        )

        for line in process.stdout:
            print(line, end="")

        process.wait()

        if process.returncode != 0:
            print(f"❌ Tuner exited with code {process.returncode}")
            sys.exit(process.returncode)

    except KeyboardInterrupt:
        print("\n⚠ Tuning interrupted by user.")
        sys.exit(0)
    except Exception as e:
        print(f"❌ Error running tuner: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
