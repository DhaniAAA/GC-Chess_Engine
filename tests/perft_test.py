#!/usr/bin/env python3
"""
Perft Test Suite for GC-Engine
Validates move generation correctness against known perft values.

Usage:
    python perft_test.py [--depth N] [--divide] [--position FEN]

GitHub Actions:
    python perft_test.py --depth 5 --all-positions
"""

import subprocess
import sys
import argparse
import os
from dataclasses import dataclass
from typing import Optional, Dict, List

# ============================================================================
# Known Perft Values
# Source: https://www.chessprogramming.org/Perft_Results
# ============================================================================

@dataclass
class PerftPosition:
    name: str
    fen: str
    expected: Dict[int, int]  # depth -> expected nodes

PERFT_POSITIONS = [
    PerftPosition(
        name="Starting Position",
        fen="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        expected={
            1: 20,
            2: 400,
            3: 8902,
            4: 197281,
            5: 4865609,
            6: 119060324,
        }
    ),
    PerftPosition(
        name="Kiwipete",
        fen="r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        expected={
            1: 48,
            2: 2039,
            3: 97862,
            4: 4085603,
            5: 193690690,
        }
    ),
    PerftPosition(
        name="Position 3",
        fen="8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        expected={
            1: 14,
            2: 191,
            3: 2812,
            4: 43238,
            5: 674624,
            6: 11030083,
        }
    ),
    PerftPosition(
        name="Position 4",
        fen="r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        expected={
            1: 6,
            2: 264,
            3: 9467,
            4: 422333,
            5: 15833292,
        }
    ),
    PerftPosition(
        name="Position 5",
        fen="rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        expected={
            1: 44,
            2: 1486,
            3: 62379,
            4: 2103487,
            5: 89941194,
        }
    ),
    PerftPosition(
        name="Position 6",
        fen="r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        expected={
            1: 46,
            2: 2079,
            3: 89890,
            4: 3894594,
            5: 164075551,
        }
    ),
]

# ============================================================================
# Engine Communication
# ============================================================================

def find_engine() -> str:
    """Find the engine executable."""
    possible_paths = [
        "./output/main.exe",
        "./output/main",
        "../output/main.exe",
        "../output/main",
        "output/main.exe",
        "output/main",
        "main.exe",
        "main",
    ]

    for path in possible_paths:
        if os.path.exists(path):
            return path

    raise FileNotFoundError("Could not find engine executable. Please compile first.")


def run_perft(engine_path: str, fen: str, depth: int, divide: bool = False) -> int:
    """Run perft on the engine and return the node count."""
    command = "divide" if divide else "perft"

    commands = f"position fen {fen}\n{command} {depth}\nquit\n"

    try:
        result = subprocess.run(
            [engine_path],
            input=commands,
            capture_output=True,
            text=True,
            timeout=300  # 5 minute timeout
        )

        output = result.stdout

        # Parse the node count from output
        for line in output.split('\n'):
            if 'Nodes:' in line:
                parts = line.split()
                for i, part in enumerate(parts):
                    if part == 'Nodes:' and i + 1 < len(parts):
                        return int(parts[i + 1])

        # If divide, the total is at the end
        if divide:
            print(output)

        return -1

    except subprocess.TimeoutExpired:
        print(f"Error: Timeout after 5 minutes for depth {depth}")
        return -1
    except Exception as e:
        print(f"Error running engine: {e}")
        return -1


def run_divide(engine_path: str, fen: str, depth: int) -> Dict[str, int]:
    """Run divide on the engine and return move-node pairs."""
    commands = f"position fen {fen}\ndivide {depth}\nquit\n"

    try:
        result = subprocess.run(
            [engine_path],
            input=commands,
            capture_output=True,
            text=True,
            timeout=300
        )

        output = result.stdout
        moves = {}

        for line in output.split('\n'):
            line = line.strip()
            if ':' in line and not line.startswith('Nodes') and not line.startswith('Time') and not line.startswith('NPS'):
                parts = line.split(':')
                if len(parts) == 2:
                    move = parts[0].strip()
                    try:
                        nodes = int(parts[1].strip())
                        moves[move] = nodes
                    except ValueError:
                        pass

        return moves

    except Exception as e:
        print(f"Error running divide: {e}")
        return {}

# ============================================================================
# Test Runner
# ============================================================================

def test_position(engine_path: str, position: PerftPosition, max_depth: int, verbose: bool = True) -> bool:
    """Test a single position up to max_depth."""
    if verbose:
        print(f"\n{'='*60}")
        print(f"Testing: {position.name}")
        print(f"FEN: {position.fen}")
        print(f"{'='*60}")

    all_passed = True

    for depth in range(1, max_depth + 1):
        if depth not in position.expected:
            if verbose:
                print(f"  Depth {depth}: No expected value, skipping")
            continue

        expected = position.expected[depth]
        actual = run_perft(engine_path, position.fen, depth)

        if actual == expected:
            status = "✅ PASS"
        else:
            status = "❌ FAIL"
            all_passed = False

        if verbose:
            print(f"  Depth {depth}: {actual:>12,} vs {expected:>12,} expected  {status}")

        if actual != expected:
            if verbose:
                print(f"           Difference: {actual - expected:+,} nodes")
            break  # Stop on first failure for this position

    return all_passed


def run_all_tests(engine_path: str, max_depth: int, verbose: bool = True) -> bool:
    """Run perft tests on all positions."""
    print(f"\n{'#'*60}")
    print(f"# GC-Engine Perft Test Suite")
    print(f"# Max Depth: {max_depth}")
    print(f"{'#'*60}")

    all_passed = True
    passed_count = 0
    failed_count = 0

    for position in PERFT_POSITIONS:
        if test_position(engine_path, position, max_depth, verbose):
            passed_count += 1
        else:
            failed_count += 1
            all_passed = False

    print(f"\n{'='*60}")
    print(f"RESULTS: {passed_count} passed, {failed_count} failed")
    print(f"{'='*60}")

    return all_passed


def compare_divide(engine_path: str, fen: str, depth: int, reference_moves: Dict[str, int]) -> None:
    """Compare divide output with reference values."""
    print(f"\nRunning divide at depth {depth}...")
    actual_moves = run_divide(engine_path, fen, depth)

    print(f"\n{'Move':<10} {'Actual':>12} {'Expected':>12} {'Status':<8}")
    print("-" * 50)

    all_moves = set(actual_moves.keys()) | set(reference_moves.keys())

    for move in sorted(all_moves):
        actual = actual_moves.get(move, 0)
        expected = reference_moves.get(move, 0)

        if actual == expected:
            status = "✅"
        elif move not in actual_moves:
            status = "❌ MISSING"
        elif move not in reference_moves:
            status = "❌ EXTRA"
        else:
            status = f"❌ {actual - expected:+d}"

        print(f"{move:<10} {actual:>12,} {expected:>12,} {status}")

# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Perft Test Suite for GC-Engine")
    parser.add_argument('--depth', '-d', type=int, default=5, help='Maximum depth to test (default: 5)')
    parser.add_argument('--all-positions', '-a', action='store_true', help='Test all known positions')
    parser.add_argument('--position', '-p', type=int, default=None, help='Test specific position (0-5)')
    parser.add_argument('--fen', '-f', type=str, default=None, help='Test custom FEN position')
    parser.add_argument('--divide', action='store_true', help='Run divide instead of perft')
    parser.add_argument('--engine', '-e', type=str, default=None, help='Path to engine executable')
    parser.add_argument('--quiet', '-q', action='store_true', help='Only output failures')

    args = parser.parse_args()

    # Find engine
    try:
        engine_path = args.engine if args.engine else find_engine()
        print(f"Using engine: {engine_path}")
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)

    # Test engine is working
    test_result = run_perft(engine_path, PERFT_POSITIONS[0].fen, 1)
    if test_result != 20:
        print(f"Error: Engine returned {test_result} for depth 1 perft (expected 20)")
        sys.exit(1)

    # Run tests
    if args.fen:
        # Test custom FEN
        if args.divide:
            moves = run_divide(engine_path, args.fen, args.depth)
            print(f"\nDivide results for depth {args.depth}:")
            for move, nodes in sorted(moves.items()):
                print(f"  {move}: {nodes:,}")
            print(f"\nTotal: {sum(moves.values()):,} nodes")
        else:
            nodes = run_perft(engine_path, args.fen, args.depth)
            print(f"\nPerft {args.depth}: {nodes:,} nodes")

    elif args.position is not None:
        # Test specific position
        if 0 <= args.position < len(PERFT_POSITIONS):
            position = PERFT_POSITIONS[args.position]

            if args.divide:
                moves = run_divide(engine_path, position.fen, args.depth)
                print(f"\n{position.name}")
                print(f"FEN: {position.fen}")
                print(f"\nDivide results for depth {args.depth}:")
                for move, nodes in sorted(moves.items()):
                    print(f"  {move}: {nodes:,}")
                print(f"\nTotal: {sum(moves.values()):,} nodes")
            else:
                passed = test_position(engine_path, position, args.depth, not args.quiet)
                sys.exit(0 if passed else 1)
        else:
            print(f"Error: Invalid position index. Use 0-{len(PERFT_POSITIONS)-1}")
            sys.exit(1)

    else:
        # Test all positions
        passed = run_all_tests(engine_path, args.depth, not args.quiet)
        sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
