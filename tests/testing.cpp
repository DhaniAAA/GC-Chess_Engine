#include "testing.hpp"
#include "eval.hpp"
#include <iostream>
#include <algorithm>

// ============================================================================
// Perft Testing Implementation
// ============================================================================

namespace PerftTest {

U64 perft(Board& board, int depth) {
    if (depth == 0) return 1;

    U64 nodes = 0;
    MoveList moves;
    MoveGen::generate_all(board, moves);

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        if (!MoveGen::is_legal(board, m)) continue;

        StateInfo si;
        board.do_move(m, si);
        nodes += perft(board, depth - 1);
        board.undo_move(m);
    }

    return nodes;
}

U64 perft_divide(Board& board, int depth) {
    if (depth == 0) return 1;

    U64 totalNodes = 0;
    MoveList moves;
    MoveGen::generate_all(board, moves);

    std::cout << "\nPerft divide at depth " << depth << ":\n";
    std::cout << "----------------------------\n";

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        if (!MoveGen::is_legal(board, m)) continue;

        StateInfo si;
        board.do_move(m, si);
        U64 nodes = perft(board, depth - 1);
        board.undo_move(m);

        std::cout << move_to_string(m) << ": " << nodes << "\n";
        totalNodes += nodes;
    }

    std::cout << "----------------------------\n";
    std::cout << "Total: " << totalNodes << "\n";

    return totalNodes;
}

std::vector<PerftPosition> get_standard_positions() {
    std::vector<PerftPosition> positions;

    // Position 1: Starting position
    positions.push_back(PerftPosition(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "Starting Position",
        {1, 20, 400, 8902, 197281, 4865609, 119060324}
    ));

    // Position 2: Kiwipete
    positions.push_back(PerftPosition(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "Kiwipete",
        {1, 48, 2039, 97862, 4085603, 193690690}
    ));

    // Position 3: En passant + castling
    positions.push_back(PerftPosition(
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "Position 3 (complex)",
        {1, 6, 264, 9467, 422333, 15833292}
    ));

    // Position 4: Mirror of Position 3
    positions.push_back(PerftPosition(
        "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
        "Position 4 (mirror)",
        {1, 6, 264, 9467, 422333, 15833292}
    ));

    // Position 5: Promotion heavy
    positions.push_back(PerftPosition(
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "Position 5 (promotions)",
        {1, 44, 1486, 62379, 2103487, 89941194}
    ));

    // Position 6: Complex middlegame
    positions.push_back(PerftPosition(
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "Position 6 (middlegame)",
        {1, 46, 2079, 89890, 3894594, 164075551}
    ));

    return positions;
}

TestResult run_position(const PerftPosition& pos, int maxDepth) {
    Board board(pos.fen);
    TestResult result;
    result.name = pos.description;
    result.passed = true;

    std::ostringstream msg;
    auto startTotal = std::chrono::high_resolution_clock::now();

    int actualMaxDepth = std::min(maxDepth, (int)pos.expectedCounts.size() - 1);

    for (int depth = 1; depth <= actualMaxDepth; ++depth) {
        U64 nodes = perft(board, depth);

        result.nodes += nodes;

        if (nodes != pos.expectedCounts[depth]) {
            result.passed = false;
            msg << "Depth " << depth << ": got " << nodes
                << ", expected " << pos.expectedCounts[depth] << "; ";
        }
    }

    auto endTotal = std::chrono::high_resolution_clock::now();
    result.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTotal - startTotal).count();

    result.message = result.passed ? "PASSED" : msg.str();
    return result;
}

std::vector<TestResult> run_suite(int maxDepth) {
    std::vector<TestResult> results;
    auto positions = get_standard_positions();

    std::cout << "\n=== Running Perft Test Suite ===\n\n";

    for (const auto& pos : positions) {
        std::cout << "Testing: " << pos.description << "... ";
        std::cout.flush();

        TestResult result = run_position(pos, maxDepth);
        results.push_back(result);

        std::cout << (result.passed ? "PASSED" : "FAILED")
                  << " (" << result.timeMs << " ms)\n";

        if (!result.passed) {
            std::cout << "  Error: " << result.message << "\n";
        }
    }

    // Summary
    int passed = std::count_if(results.begin(), results.end(),
        [](const TestResult& r) { return r.passed; });

    std::cout << "\nPerft Summary: " << passed << "/" << results.size()
              << " positions passed\n";

    return results;
}

}  // namespace PerftTest
