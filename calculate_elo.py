#!/usr/bin/env python3
"""
ELO Calculator for Chess Engine Testing
Calculates estimated ELO based on gauntlet results.
"""

import re
import os
import sys
import math
from pathlib import Path

# Known opponent ELO ratings (approximate)
# You can update these based on actual ratings from CCRL
OPPONENT_ELO = {
    "main-old-2": 1996,
    "main": 1996,
}

def get_opponent_elo(opponent_name):
    """Get estimated ELO for opponent engine."""
    name_lower = opponent_name.lower()

    for key, elo in OPPONENT_ELO.items():
        # Case-insensitive match
        if key.lower() in name_lower:
            return elo

    # If not found, ask user or use default
    print(f"  Unknown opponent: {opponent_name}")
    try:
        elo = input(f"  Enter estimated ELO for {opponent_name} (or press Enter for 1500): ")
        return int(elo) if elo.strip() else 1500
    except:
        return 1500

def calculate_elo_difference(score_percentage):
    """
    Calculate ELO difference from score percentage.
    Uses the formula: ELO_diff = 400 * log10(score / (1 - score))
    """
    if score_percentage <= 0:
        return -800  # Very bad, cap at -800
    elif score_percentage >= 1:
        return 800   # Very good, cap at +800
    else:
        return 400 * math.log10(score_percentage / (1 - score_percentage))

def parse_pgn_results(pgn_file):
    """Parse a PGN file to extract W-L-D results."""
    wins = 0
    losses = 0
    draws = 0

    with open(pgn_file, 'r') as f:
        content = f.read()

    # Find all Result tags
    results = re.findall(r'\[Result\s+"([^"]+)"\]', content)

    # Find which color was GC-Engine
    white_tags = re.findall(r'\[White\s+"([^"]+)"\]', content)

    for i, result in enumerate(results):
        is_white = i < len(white_tags) and "GC-Engine" in white_tags[i]

        if result == "1-0":
            if is_white:
                wins += 1
            else:
                losses += 1
        elif result == "0-1":
            if is_white:
                losses += 1
            else:
                wins += 1
        elif result == "1/2-1/2":
            draws += 1

    return wins, losses, draws

def main():
    print("=" * 60)
    print("  ELO Calculator for GC-Engine")
    print("=" * 60)
    print()

    # Find all result files
    result_files = list(Path(".").glob("results_vs_*.pgn"))

    if not result_files:
        print("No result files found (results_vs_*.pgn)")
        print("Please run gauntlet tests first with run_gauntlet.bat")
        return

    total_score = 0
    total_games = 0
    total_weighted_elo = 0

    results = []

    for pgn_file in result_files:
        # Extract opponent name from filename
        match = re.search(r'results_vs_(.+)\.pgn', str(pgn_file))
        if not match:
            continue

        opponent_name = match.group(1)
        wins, losses, draws = parse_pgn_results(pgn_file)
        games = wins + losses + draws

        if games == 0:
            continue

        score = wins + draws * 0.5
        score_pct = score / games
        opponent_elo = get_opponent_elo(opponent_name)
        elo_diff = calculate_elo_difference(score_pct)
        performance_elo = opponent_elo + elo_diff

        results.append({
            'opponent': opponent_name,
            'wins': wins,
            'losses': losses,
            'draws': draws,
            'games': games,
            'score': score,
            'score_pct': score_pct,
            'opponent_elo': opponent_elo,
            'elo_diff': elo_diff,
            'performance_elo': performance_elo
        })

        total_score += score
        total_games += games
        total_weighted_elo += opponent_elo * games

    if not results:
        print("No valid results found.")
        return

    # Print results
    print("-" * 60)
    print(f"{'Opponent':<25} {'W-L-D':<12} {'Score':<8} {'Opp ELO':<8} {'Perf':<8}")
    print("-" * 60)

    for r in results:
        wld = f"{r['wins']}-{r['losses']}-{r['draws']}"
        score_str = f"{r['score_pct']*100:.1f}%"
        print(f"{r['opponent']:<25} {wld:<12} {score_str:<8} {r['opponent_elo']:<8} {r['performance_elo']:.0f}")

    print("-" * 60)

    # Calculate overall performance
    overall_score_pct = total_score / total_games
    avg_opponent_elo = total_weighted_elo / total_games
    overall_elo_diff = calculate_elo_difference(overall_score_pct)
    estimated_elo = avg_opponent_elo + overall_elo_diff

    print()
    print(f"Total Games:        {total_games}")
    print(f"Total Score:        {total_score:.1f} / {total_games} ({overall_score_pct*100:.1f}%)")
    print(f"Avg Opponent ELO:   {avg_opponent_elo:.0f}")
    print(f"ELO Difference:     {overall_elo_diff:+.0f}")
    print()
    print("=" * 60)
    print(f"  ESTIMATED ELO: {estimated_elo:.0f}")
    print("=" * 60)
    print()
    print("Note: This is a rough estimate. For accurate ratings, use more")
    print("games (100+ per opponent) and opponents with known CCRL ratings.")
    print()
    print("CCRL Rating Lists: https://computerchess.org.uk/ccrl/")

if __name__ == "__main__":
    main()
