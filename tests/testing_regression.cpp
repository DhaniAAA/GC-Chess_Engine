#include "testing.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>

// ============================================================================
// Regression Testing / SPRT Implementation
// ============================================================================

namespace RegressionTest {

std::string generate_cutechess_command(const MatchConfig& config) {
    std::ostringstream cmd;

    cmd << "cutechess-cli";

    // Engine 1
    cmd << " -engine name=\"" << config.engine1Name << "\"";
    cmd << " cmd=\"" << config.engine1Path << "\"";
    cmd << " option.Hash=" << config.hashMB;
    cmd << " option.Threads=" << config.threads;

    // Engine 2
    cmd << " -engine name=\"" << config.engine2Name << "\"";
    cmd << " cmd=\"" << config.engine2Path << "\"";
    cmd << " option.Hash=" << config.hashMB;
    cmd << " option.Threads=" << config.threads;

    // Match settings
    cmd << " -each proto=uci tc="
        << (config.timeControlMs / 1000.0) << "+"
        << (config.incrementMs / 1000.0);

    cmd << " -games " << config.games;
    cmd << " -rounds " << (config.games / 2);
    cmd << " -repeat";
    cmd << " -recover";

    // Opening book
    if (!config.bookPath.empty()) {
        cmd << " -openings file=\"" << config.bookPath << "\" format=pgn order=random";
    }

    // Adjudication
    if (config.adjudicateDraw) {
        cmd << " -draw movenumber=" << config.drawMoveLimit
            << " movecount=8 score=" << config.drawScoreLimit;
    }
    if (config.adjudicateMate) {
        cmd << " -resign movecount=3 score=1000";
    }

    // Output
    if (!config.outputPgn.empty()) {
        cmd << " -pgnout \"" << config.outputPgn << "\"";
    }

    cmd << " -concurrency " << config.threads;

    return cmd.str();
}

std::string generate_fastchess_command(const MatchConfig& config) {
    std::ostringstream cmd;

    cmd << "fastchess";

    // Engine 1
    cmd << " -engine cmd=\"" << config.engine1Path << "\"";
    cmd << " name=\"" << config.engine1Name << "\"";
    cmd << " option.Hash=" << config.hashMB;

    // Engine 2
    cmd << " -engine cmd=\"" << config.engine2Path << "\"";
    cmd << " name=\"" << config.engine2Name << "\"";
    cmd << " option.Hash=" << config.hashMB;

    // Time control
    cmd << " -each tc=" << (config.timeControlMs / 1000.0)
        << "+" << (config.incrementMs / 1000.0);

    cmd << " -rounds " << (config.games / 2);
    cmd << " -repeat";
    cmd << " -recover";

    if (!config.bookPath.empty()) {
        cmd << " -openings file=\"" << config.bookPath << "\"";
    }

    cmd << " -concurrency " << config.threads;

    return cmd.str();
}

double calculate_elo_from_score(double score, int games) {
    if (score <= 0 || score >= 1) return 0;
    return -400 * std::log10(1.0 / score - 1.0);
}

bool check_sprt(int wins, int losses, int draws, double elo0, double elo1,
                double alpha, double beta) {
    // SPRT (Sequential Probability Ratio Test)
    // H0: ELO difference = elo0
    // H1: ELO difference = elo1

    int games = wins + losses + draws;
    if (games < 10) return false;  // Not enough data

    double score = (wins + draws * 0.5) / games;

    // Convert ELO to expected score
    double s0 = 1.0 / (1.0 + std::pow(10.0, -elo0 / 400.0));
    double s1 = 1.0 / (1.0 + std::pow(10.0, -elo1 / 400.0));

    // Log-likelihood ratio
    double llr = 0;
    if (wins > 0) llr += wins * std::log(score / s0 * (1 - s0) / (1 - score));
    if (losses > 0) llr += losses * std::log((1 - score) / (1 - s0) * s0 / score);
    if (draws > 0) llr += draws * std::log(0.5 / s0 * (1 - s0) / 0.5);

    // Threshold values
    double lowerBound = std::log(beta / (1 - alpha));
    double upperBound = std::log((1 - beta) / alpha);

    return llr >= upperBound;  // H1 accepted (improvement detected)
}

MatchConfig create_sprt_config(const std::string& engine1, const std::string& engine2,
                                double elo0, double elo1) {
    MatchConfig config;

    config.engine1Path = engine1;
    config.engine2Path = engine2;
    config.engine1Name = "New";
    config.engine2Name = "Baseline";

    // SPRT settings - run many games
    config.games = 1000;
    config.timeControlMs = 10000;  // 10+0.1
    config.incrementMs = 100;
    config.hashMB = 64;
    config.threads = 1;

    config.adjudicateDraw = true;
    config.adjudicateMate = true;
    config.drawMoveLimit = 50;
    config.drawScoreLimit = 10;

    return config;
}

MatchConfig create_gauntlet_config(const std::string& testedEngine,
                                    const std::vector<std::string>& opponents) {
    // Returns config for first opponent - caller should iterate
    MatchConfig config;

    config.engine1Path = testedEngine;
    config.engine1Name = "Tested";

    if (!opponents.empty()) {
        config.engine2Path = opponents[0];
        config.engine2Name = "Opponent_1";
    }

    config.games = 100;
    config.timeControlMs = 5000;  // Faster for gauntlet
    config.incrementMs = 50;

    return config;
}

void generate_test_script(const MatchConfig& config, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not create script " << filename << "\n";
        return;
    }

    // Windows batch script
    file << "@echo off\n";
    file << "REM Auto-generated SPRT test script\n";
    file << "REM Generated for: " << config.engine1Name << " vs " << config.engine2Name << "\n\n";

    file << "echo Running SPRT test...\n";
    file << "echo Games: " << config.games << "\n";
    file << "echo Time Control: " << (config.timeControlMs/1000.0) << "+"
         << (config.incrementMs/1000.0) << "\n\n";

    file << generate_cutechess_command(config) << "\n\n";

    file << "echo.\n";
    file << "echo Test complete!\n";
    file << "pause\n";

    std::cout << "Generated test script: " << filename << "\n";
}

}  // namespace RegressionTest

// ============================================================================
// Test Runner Implementation
// ============================================================================

namespace TestRunner {

void run_all_tests() {
    std::cout << "\n";
    std::cout << "============================================\n";
    std::cout << "      Chess Engine Complete Test Suite      \n";
    std::cout << "============================================\n\n";

    run_perft_tests(5);
    run_tactical_tests(10, 5000);
    run_benchmark(10);

    std::cout << "\n============================================\n";
    std::cout << "           All Tests Completed!             \n";
    std::cout << "============================================\n";
}

void run_perft_tests(int maxDepth) {
    PerftTest::run_suite(maxDepth);
}

void run_tactical_tests(int depth, int64_t timeMs) {
    std::cout << "\n--- Win At Chess Positions ---\n";
    auto wacResults = TacticalTest::run_suite(TacticalTest::get_wac_positions(), depth, timeMs);

    std::cout << "\n--- Bratko-Kopec Positions ---\n";
    auto bkResults = TacticalTest::run_suite(TacticalTest::get_bratko_kopec_positions(), depth, timeMs);

    std::cout << "\n--- Mate Puzzles ---\n";
    auto mateResults = TacticalTest::run_suite(TacticalTest::get_mate_positions(), depth, timeMs);
}

void run_benchmark(int depth) {
    BenchTest::run_depth_benchmark(depth);
}

void generate_report(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not create report " << filename << "\n";
        return;
    }

    file << "Chess Engine Test Report\n";
    file << "========================\n\n";
    file << "Generated: " << __DATE__ << " " << __TIME__ << "\n\n";

    // Add test results here
    file << "See console output for detailed results.\n";
}

void print_summary() {
    std::cout << "\n=== Test Suite Summary ===\n";
    std::cout << "Run 'test perft' for move generation tests\n";
    std::cout << "Run 'test tactical' for tactical puzzle tests\n";
    std::cout << "Run 'test bench' for performance benchmark\n";
    std::cout << "Run 'test all' for complete test suite\n";
}

}  // namespace TestRunner
