#include "testing.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>

// ============================================================================
// Benchmark Testing Implementation
// ============================================================================

namespace BenchTest {

std::vector<std::string> get_benchmark_positions() {
    return {
        // Starting position
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        // Sicilian Defense
        "r1b1k2r/2qnbppp/p2ppn2/1p4B1/3NPPP1/2N2Q2/PPP4P/2KR1B1R w kq - 0 11",
        // Queen's Gambit
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 2 4",
        // Kiwipete
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        // Complex middlegame
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        // Promotion heavy
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        // Endgame
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        // Rook endgame
        "8/k7/3p4/p2P1p2/P2P1P2/8/8/K7 w - - 0 1",
        // Tactical
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        // Quiet position
        "r3k2r/pbpnqp2/1p1ppn1p/6p1/2PP4/2PBPN2/P4PPP/R1BQK2R w KQkq - 0 10",
        // King safety
        "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/5N1P/PPBN1PP1/R1BQR1K1 w - - 0 13",
        // Double pawns
        "r2qr1k1/pp1nbppp/2p2n2/3p2B1/3P4/2NBP3/PPQ2PPP/R3K2R w KQ - 5 12",
        // Open file
        "r1bq1rk1/pp3ppp/2nbpn2/3p4/2PP4/1PN1PN2/1B3PPP/R2QKB1R w KQ - 0 9",
        // Semi-closed
        "r1bqkb1r/pp1n1ppp/2p1pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w KQkq - 0 6",
        // Exchange variation
        "r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 7"
    };
}

BenchmarkResult run_depth_benchmark(int depth) {
    BenchmarkResult result;
    result.name = "Depth " + std::to_string(depth) + " Benchmark";

    auto positions = get_benchmark_positions();

    std::cout << "\n=== Running Depth Benchmark ===\n";
    std::cout << "Depth: " << depth << ", Positions: " << positions.size() << "\n\n";

    auto startTotal = std::chrono::steady_clock::now();

    for (size_t i = 0; i < positions.size(); ++i) {
        Board board(positions[i]);

        SearchLimits limits;
        limits.depth = depth;

        std::cout << "[" << (i + 1) << "/" << positions.size() << "] Searching... ";
        std::cout.flush();

        auto start = std::chrono::steady_clock::now();
        Searcher.start(board, limits);
        auto end = std::chrono::steady_clock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        U64 nodes = Searcher.stats().nodes;

        result.totalNodes += nodes;
        ++result.positionsRun;

        std::cout << nodes << " nodes, " << elapsed << " ms, "
                  << (elapsed > 0 ? nodes * 1000 / elapsed : nodes) << " nps\n";
    }

    auto endTotal = std::chrono::steady_clock::now();
    result.totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTotal - startTotal).count();

    result.nps = result.totalTimeMs > 0 ?
        (double)result.totalNodes * 1000.0 / result.totalTimeMs : 0;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Total Nodes: " << result.totalNodes << "\n";
    std::cout << "Total Time: " << result.totalTimeMs << " ms\n";
    std::cout << "Average NPS: " << (U64)result.nps << "\n";

    return result;
}

BenchmarkResult run_time_benchmark(int64_t msPerPosition) {
    BenchmarkResult result;
    result.name = "Time " + std::to_string(msPerPosition) + "ms Benchmark";

    auto positions = get_benchmark_positions();

    std::cout << "\n=== Running Time Benchmark ===\n";
    std::cout << "Time per position: " << msPerPosition << " ms\n\n";

    auto startTotal = std::chrono::steady_clock::now();

    for (size_t i = 0; i < positions.size(); ++i) {
        Board board(positions[i]);

        SearchLimits limits;
        limits.movetime = msPerPosition;

        std::cout << "[" << (i + 1) << "/" << positions.size() << "] ";
        std::cout.flush();

        Searcher.start(board, limits);

        result.totalNodes += Searcher.stats().nodes;
        ++result.positionsRun;

        std::cout << "SelDepth " << Searcher.stats().selDepth << ", "
                  << Searcher.stats().nodes << " nodes\n";
    }

    auto endTotal = std::chrono::steady_clock::now();
    result.totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTotal - startTotal).count();

    result.nps = result.totalTimeMs > 0 ?
        (double)result.totalNodes * 1000.0 / result.totalTimeMs : 0;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Total Nodes: " << result.totalNodes << "\n";
    std::cout << "Total Time: " << result.totalTimeMs << " ms\n";
    std::cout << "Average NPS: " << (U64)result.nps << "\n";

    return result;
}

BenchmarkResult run_node_benchmark(U64 nodesPerPosition) {
    BenchmarkResult result;
    result.name = "Node " + std::to_string(nodesPerPosition) + " Benchmark";

    auto positions = get_benchmark_positions();

    std::cout << "\n=== Running Node Benchmark ===\n";
    std::cout << "Nodes per position: " << nodesPerPosition << "\n\n";

    auto startTotal = std::chrono::steady_clock::now();

    for (size_t i = 0; i < positions.size(); ++i) {
        Board board(positions[i]);

        SearchLimits limits;
        limits.nodes = nodesPerPosition;

        std::cout << "[" << (i + 1) << "/" << positions.size() << "] ";
        std::cout.flush();

        Searcher.start(board, limits);

        result.totalNodes += Searcher.stats().nodes;
        ++result.positionsRun;

        std::cout << "SelDepth " << Searcher.stats().selDepth << "\n";
    }

    auto endTotal = std::chrono::steady_clock::now();
    result.totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTotal - startTotal).count();

    result.nps = result.totalTimeMs > 0 ?
        (double)result.totalNodes * 1000.0 / result.totalTimeMs : 0;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Total Nodes: " << result.totalNodes << "\n";
    std::cout << "Total Time: " << result.totalTimeMs << " ms\n";
    std::cout << "Average NPS: " << (U64)result.nps << "\n";

    return result;
}

double compare_benchmark(const BenchmarkResult& current, const BenchmarkResult& baseline) {
    if (baseline.nps == 0) return 0;
    return ((current.nps - baseline.nps) / baseline.nps) * 100.0;
}

void save_result(const BenchmarkResult& result, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not save benchmark to " << filename << "\n";
        return;
    }

    file << "name=" << result.name << "\n";
    file << "nodes=" << result.totalNodes << "\n";
    file << "time=" << result.totalTimeMs << "\n";
    file << "nps=" << result.nps << "\n";
    file << "positions=" << result.positionsRun << "\n";
}

BenchmarkResult load_result(const std::string& filename) {
    BenchmarkResult result;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Error: Could not load benchmark from " << filename << "\n";
        return result;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "name") result.name = value;
        else if (key == "nodes") result.totalNodes = std::stoull(value);
        else if (key == "time") result.totalTimeMs = std::stoll(value);
        else if (key == "nps") result.nps = std::stod(value);
        else if (key == "positions") result.positionsRun = std::stoi(value);
    }

    return result;
}

}  // namespace BenchTest
