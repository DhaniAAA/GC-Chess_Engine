#include "testing.hpp"
#include "eval.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>

// ============================================================================
// Tactical Test Implementation
// ============================================================================

namespace TacticalTest {

TacticalPosition parse_epd_line(const std::string& line) {
    TacticalPosition pos;
    if (line.empty() || line[0] == '#') return pos;

    // EPD format: <fen> <operations>
    // Example: 1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - bm Qd1+; id "WAC.001";

    std::istringstream iss(line);
    std::string fenPart;

    // Read FEN (first 4 parts minimum)
    std::string board, turn, castling, ep;
    if (!(iss >> board >> turn >> castling >> ep)) return pos;

    pos.fen = board + " " + turn + " " + castling + " " + ep + " 0 1";

    // Parse operations manually (no regex for MinGW compatibility)
    std::string rest;
    std::getline(iss, rest);

    // Find bm (best move)
    size_t bmPos = rest.find("bm ");
    if (bmPos != std::string::npos) {
        size_t start = bmPos + 3;
        size_t end = rest.find(';', start);
        if (end != std::string::npos) {
            std::string moves = rest.substr(start, end - start);
            std::istringstream moveStream(moves);
            std::string move;
            while (moveStream >> move) {
                pos.bestMoves.push_back(move);
            }
        }
    }

    // Find am (avoid move)
    size_t amPos = rest.find("am ");
    if (amPos != std::string::npos) {
        size_t start = amPos + 3;
        size_t end = rest.find(';', start);
        if (end != std::string::npos) {
            std::string moves = rest.substr(start, end - start);
            std::istringstream moveStream(moves);
            std::string move;
            while (moveStream >> move) {
                pos.avoidMoves.push_back(move);
            }
        }
    }

    // Find id
    size_t idPos = rest.find("id \"");
    if (idPos != std::string::npos) {
        size_t start = idPos + 4;
        size_t end = rest.find('"', start);
        if (end != std::string::npos) {
            pos.id = rest.substr(start, end - start);
        }
    }

    return pos;
}

std::vector<TacticalPosition> load_epd(const std::string& filename) {
    std::vector<TacticalPosition> positions;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Error: Could not open EPD file: " << filename << "\n";
        return positions;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        TacticalPosition pos = parse_epd_line(line);
        if (!pos.fen.empty() && !pos.bestMoves.empty()) {
            positions.push_back(pos);
        }
    }

    return positions;
}

TestResult run_position(const TacticalPosition& pos, int depth, int64_t timeMs) {
    TestResult result;
    result.name = pos.id.empty() ? pos.fen.substr(0, 30) + "..." : pos.id;

    Board board(pos.fen);

    SearchLimits limits;
    limits.depth = depth;
    limits.movetime = timeMs;

    auto start = std::chrono::high_resolution_clock::now();
    Searcher.start(board, limits);
    auto end = std::chrono::high_resolution_clock::now();

    result.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    result.nodes = Searcher.stats().nodes;

    Move bestMove = Searcher.best_move();
    std::string moveStr = move_to_string(bestMove);

    // Check if found move is in best moves list
    result.passed = false;
    for (const auto& bm : pos.bestMoves) {
        // Compare moves (handle different notations)
        if (moveStr == bm || moveStr.find(bm) != std::string::npos) {
            result.passed = true;
            break;
        }
    }

    // Check if found move is in avoid list
    if (result.passed) {
        for (const auto& am : pos.avoidMoves) {
            if (moveStr == am) {
                result.passed = false;
                result.message = "Found avoided move: " + moveStr;
                break;
            }
        }
    }

    if (result.passed) {
        result.message = "Found: " + moveStr;
    } else {
        std::ostringstream oss;
        oss << "Expected: ";
        for (size_t i = 0; i < pos.bestMoves.size(); ++i) {
            if (i > 0) oss << "/";
            oss << pos.bestMoves[i];
        }
        oss << ", Got: " << moveStr;
        result.message = oss.str();
    }

    return result;
}

std::vector<TestResult> run_suite(const std::vector<TacticalPosition>& positions,
                                   int depth, int64_t timeMs) {
    std::vector<TestResult> results;

    std::cout << "\n=== Running Tactical Test Suite ===\n";
    std::cout << "Positions: " << positions.size() << ", Depth: " << depth
              << ", Time: " << timeMs << "ms\n\n";

    int passed = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& pos = positions[i];

        std::cout << "[" << (i + 1) << "/" << positions.size() << "] "
                  << (pos.id.empty() ? "Position" : pos.id) << "... ";
        std::cout.flush();

        TestResult result = run_position(pos, depth, timeMs);
        results.push_back(result);

        if (result.passed) {
            ++passed;
            std::cout << "PASSED";
        } else {
            std::cout << "FAILED";
        }
        std::cout << " (" << result.timeMs << "ms) - " << result.message << "\n";
    }

    std::cout << "\nTactical Summary: " << passed << "/" << positions.size()
              << " (" << (100.0 * passed / positions.size()) << "%)\n";

    return results;
}

void print_summary(const std::vector<TestResult>& results) {
    int passed = 0;
    U64 totalNodes = 0;
    int64_t totalTime = 0;

    for (const auto& r : results) {
        if (r.passed) ++passed;
        totalNodes += r.nodes;
        totalTime += r.timeMs;
    }

    std::cout << "\n=== Test Summary ===\n";
    std::cout << "Passed: " << passed << "/" << results.size()
              << " (" << (100.0 * passed / results.size()) << "%)\n";
    std::cout << "Total Nodes: " << totalNodes << "\n";
    std::cout << "Total Time: " << totalTime << " ms\n";
    std::cout << "NPS: " << (totalTime > 0 ? totalNodes * 1000 / totalTime : 0) << "\n";
}

// Built-in WAC positions (sample - first 20)
std::vector<TacticalPosition> get_wac_positions() {
    std::vector<TacticalPosition> positions;

    positions.push_back(TacticalPosition(
        "2rr3k/pp3pp1/1nnqbN1p/3pN3/2pP4/2P3Q1/PPB4P/R4RK1 w - - 0 1",
        "WAC.001", {"Qg6"}, "Zugzwang/Attack"));

    positions.push_back(TacticalPosition(
        "8/7p/5k2/5p2/p1p2P2/Pr1pPK2/1P1R3P/8 b - - 0 1",
        "WAC.002", {"Rxb2"}, "Passed pawn"));

    positions.push_back(TacticalPosition(
        "5rk1/1ppb3p/p1pb4/6q1/3P1p1r/2P1R2P/PP1BQ1P1/5RKN w - - 0 1",
        "WAC.003", {"Rg3"}, "Defense"));

    positions.push_back(TacticalPosition(
        "r1bq2rk/pp3pbp/2p1p1pQ/7P/3P4/2PB1N2/PP3PPR/2KR4 w - - 0 1",
        "WAC.004", {"Qxh7+"}, "Sacrifice"));

    positions.push_back(TacticalPosition(
        "5k2/6pp/p1qN4/1p1p4/3P4/2PKP2Q/PP3r2/3R4 b - - 0 1",
        "WAC.005", {"Qc1+"}, "Check sequence"));

    positions.push_back(TacticalPosition(
        "7k/p7/1R5K/6r1/6p1/6P1/8/8 w - - 0 1",
        "WAC.006", {"Rb8+"}, "Stalemate trick"));

    positions.push_back(TacticalPosition(
        "rnbqkb1r/pppp1ppp/8/4P3/6n1/7P/PPPNPPP1/R1BQKBNR b KQkq - 0 1",
        "WAC.007", {"Ne3"}, "Knight fork"));

    positions.push_back(TacticalPosition(
        "r4q1k/p2bR1rp/2p2Q1N/5p2/5p2/2P5/PP3PPP/R5K1 w - - 0 1",
        "WAC.008", {"Rf7"}, "Discovered attack"));

    positions.push_back(TacticalPosition(
        "3q1rk1/p4pp1/2pb3p/3p4/6Pr/1PNQ4/P1PB1PP1/4RRK1 b - - 0 1",
        "WAC.009", {"Bh2+"}, "Bishop sacrifice"));

    positions.push_back(TacticalPosition(
        "2br2k1/2q3rn/p2NppQ1/2p1P3/Pp5R/4P3/1P3PPP/3R2K1 w - - 0 1",
        "WAC.010", {"Rxh7"}, "Rook sacrifice"));

    return positions;
}

// Built-in Bratko-Kopec positions
std::vector<TacticalPosition> get_bratko_kopec_positions() {
    std::vector<TacticalPosition> positions;

    positions.push_back(TacticalPosition(
        "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1",
        "BK.01", {"Qd1+"}, "Queen sacrifice for mate"));

    positions.push_back(TacticalPosition(
        "3r1k2/4npp1/1ppr3p/p6P/P2PPPP1/1NR5/5K2/2R5 w - - 0 1",
        "BK.02", {"d5"}, "Central breakthrough"));

    positions.push_back(TacticalPosition(
        "2q1rr1k/3bbnnp/p2p1pp1/2pPp3/PpP1P1P1/1P2BNNP/2BQ1PRK/7R b - - 0 1",
        "BK.03", {"f5"}, "Pawn break"));

    positions.push_back(TacticalPosition(
        "rnbqkb1r/p3pppp/1p6/2ppP3/3N4/2P5/PPP1QPPP/R1B1KB1R w KQkq - 0 1",
        "BK.04", {"e6"}, "Pawn push"));

    positions.push_back(TacticalPosition(
        "r1b2rk1/2q1b1pp/p2ppn2/1p6/3QP3/1BN1B3/PPP3PP/R4RK1 w - - 0 1",
        "BK.05", {"Nd5", "a4"}, "Knight maneuver"));

    return positions;
}

// Mate puzzles
std::vector<TacticalPosition> get_mate_positions() {
    std::vector<TacticalPosition> positions;

    // Mate in 1
    positions.push_back(TacticalPosition(
        "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4",
        "M1.01", {"Qxf7#"}, "Scholar's mate", 1));

    // Mate in 2
    positions.push_back(TacticalPosition(
        "r2qkb1r/pp2nppp/3p4/2pNN1B1/2BnP3/3P4/PPP2PPP/R2bK2R w KQkq - 0 1",
        "M2.01", {"Nf6+"}, "Double knight", 2));

    positions.push_back(TacticalPosition(
        "1rb4r/pkPp3p/1b1P3n/1Q6/N3Pp2/8/P1P3PP/7K w - - 0 1",
        "M2.02", {"Qb4", "Qd5+"}, "Queen penetration", 2));

    // Mate in 3
    positions.push_back(TacticalPosition(
        "6k1/pp4p1/2p5/2bp4/8/P5Pb/1P3rrP/2BRRN1K b - - 0 1",
        "M3.01", {"Rg1+"}, "Rook sacrifice", 3));

    return positions;
}

std::vector<TacticalPosition> get_sts_positions() {
    // Strategic Test Suite - sample positions
    std::vector<TacticalPosition> positions;

    positions.push_back(TacticalPosition(
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "STS.001", {"c3", "d3"}, "Italian Game setup"));

    return positions;
}

}  // namespace TacticalTest
