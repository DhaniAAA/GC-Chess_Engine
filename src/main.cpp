#include <iostream>
#include <chrono>
#include <functional>
#include "board.hpp"
#include "bitboard.hpp"
#include "magic.hpp"
#include "zobrist.hpp"
#include "movegen.hpp"
#include "tt.hpp"
#include "moveorder.hpp"
#include "search.hpp"
#include "eval.hpp"
#include "uci.hpp"
// #include "testing.hpp"

// ============================================================================
// Test Functions - Stage 1
// ============================================================================

void test_bitboards() {
    std::cout << "=== Testing Bitboards ===\n\n";

    std::cout << "Knight attacks from E4:\n";
    std::cout << Bitboards::pretty(knight_attacks_bb(SQ_E4));

    std::cout << "Popcount of knight attacks: " << popcount(knight_attacks_bb(SQ_E4)) << "\n\n";
}

void test_magic_bitboards() {
    std::cout << "=== Testing Magic Bitboards ===\n\n";

    Bitboard blockers = square_bb(SQ_E7) | square_bb(SQ_B4);
    std::cout << "Rook attacks from E4 with blockers:\n";
    std::cout << Bitboards::pretty(rook_attacks_bb(SQ_E4, blockers));
}

// ============================================================================
// Test Functions - Stage 2
// ============================================================================

void test_move_generation() {
    std::cout << "=== Testing Move Generation ===\n\n";

    Board board;
    MoveList moves;

    MoveGen::generate_all(board, moves);
    std::cout << "Starting position - pseudo-legal moves: " << moves.size() << "\n";

    // Count legal moves
    int legalCount = 0;
    for (int i = 0; i < moves.size(); ++i) {
        if (MoveGen::is_legal(board, moves[i].move)) {
            ++legalCount;
        }
    }
    std::cout << "Legal moves: " << legalCount << "\n";

    // Print first 10 moves
    std::cout << "First 10 moves: ";
    for (int i = 0; i < std::min(10, moves.size()); ++i) {
        std::cout << move_to_string(moves[i].move) << " ";
    }
    std::cout << "\n\n";

    // Test position with en passant
    Board ep_pos("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");
    std::cout << "Position with en passant:\n";
    std::cout << ep_pos.pretty();

    moves.clear();
    MoveGen::generate_all(ep_pos, moves);

    // Find en passant move
    for (int i = 0; i < moves.size(); ++i) {
        if (moves[i].move.is_enpassant()) {
            std::cout << "En passant move found: " << move_to_string(moves[i].move) << "\n\n";
            break;
        }
    }
}

void test_transposition_table() {
    std::cout << "=== Testing Transposition Table ===\n\n";

    Board board;

    // Store an entry
    bool found;
    TTEntry* tte = TT.probe(board.key(), found);
    std::cout << "Initial probe - found: " << (found ? "yes" : "no") << "\n";

    // Save entry
    tte->save(board.key(), 50, 30, BOUND_EXACT, 6, Move::make(SQ_E2, SQ_E4), TT.generation());

    // Probe again
    tte = TT.probe(board.key(), found);
    std::cout << "After save - found: " << (found ? "yes" : "no") << "\n";
    if (found) {
        std::cout << "  Score: " << tte->score() << "\n";
        std::cout << "  Depth: " << tte->depth() << "\n";
        std::cout << "  Move: " << move_to_string(tte->move()) << "\n";
        std::cout << "  Bound: " << (tte->bound() == BOUND_EXACT ? "EXACT" :
                                     tte->bound() == BOUND_LOWER ? "LOWER" : "UPPER") << "\n";
    }
    std::cout << "\n";
}

void test_see() {
    std::cout << "=== Testing Static Exchange Evaluation ===\n\n";

    // Position where Rxd5 is winning (pawn takes rook, but we recapture)
    Board pos("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    std::cout << pos.pretty();

    // Test SEE for exd5
    Move m = Move::make(SQ_E5, SQ_D7);  // Nxd7
    int see = SEE::evaluate(pos, m);
    std::cout << "SEE for Nxd7: " << see << " (expected ~330 for bishop)\n";

    // Test another capture
    m = Move::make(SQ_D5, SQ_E6);  // dxe6
    see = SEE::evaluate(pos, m);
    std::cout << "SEE for dxe6: " << see << " (captures pawn)\n\n";
}

void test_move_ordering() {
    std::cout << "=== Testing Move Ordering ===\n\n";

    Board board;

    KillerTable kt;
    CounterMoveTable cm;
    HistoryTable ht;

    // Store some killer moves
    kt.store(0, Move::make(SQ_G1, SQ_F3));
    kt.store(0, Move::make(SQ_B1, SQ_C3));

    // Get moves in order
    MovePicker mp(board, MOVE_NONE, 0, kt, cm, ht, MOVE_NONE);

    std::cout << "Moves in priority order (first 10):\n";
    Move m;
    int count = 0;
    while ((m = mp.next_move()) != MOVE_NONE && count < 10) {
        std::cout << "  " << (count + 1) << ". " << move_to_string(m) << "\n";
        ++count;
    }
    std::cout << "\n";
}

void test_evaluation() {
    std::cout << "=== Testing Evaluation ===\n\n";

    Board board;
    int eval = Eval::evaluate(board);
    std::cout << "Starting position eval: " << eval << " cp\n";

    // Position with material advantage
    StateInfo si;
    Board white_up("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1");  // White missing knight
    eval = Eval::evaluate(white_up);
    std::cout << "White missing a knight: " << eval << " cp (expected ~-320)\n";

    // Sicilian position
    Board sicilian("r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");
    eval = Eval::evaluate(sicilian);
    std::cout << "Sicilian position: " << eval << " cp\n\n";
}

void test_search() {
    std::cout << "=== Testing Search ===\n\n";

    Board board;

    // Search to depth 6
    SearchLimits limits;
    limits.depth = 6;

    std::cout << "Searching starting position to depth " << limits.depth << "...\n\n";

    auto start = std::chrono::high_resolution_clock::now();

    Searcher.start(board, limits);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\nSearch complete!\n";
    std::cout << "Best move: " << move_to_string(Searcher.best_move()) << "\n";
    std::cout << "Nodes: " << Searcher.stats().nodes << "\n";
    std::cout << "Time: " << duration << " ms\n";
    std::cout << "NPS: " << (Searcher.stats().nodes * 1000 / (duration + 1)) << "\n\n";

    // Test a tactical position (mate in 2)
    StateInfo si;
    Board mateIn2("r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4");
    std::cout << "Scholar's Mate position:\n";
    std::cout << mateIn2.pretty();

    limits.depth = 4;
    std::cout << "Searching for mate...\n\n";

    Searcher.start(mateIn2, limits);

    std::cout << "\nBest move: " << move_to_string(Searcher.best_move()) << " (expected Qxf7#)\n\n";
}

void test_perft() {
    std::cout << "=== Perft Test (Move Generation Verification) ===\n\n";

    // Simple perft function (without TT)
    std::function<U64(Board&, int)> perft = [&](Board& b, int depth) -> U64 {
        if (depth == 0) return 1;

        U64 nodes = 0;
        MoveList moves;
        MoveGen::generate_all(b, moves);

        for (int i = 0; i < moves.size(); ++i) {
            Move m = moves[i].move;
            if (!MoveGen::is_legal(b, m)) continue;

            StateInfo si;
            b.do_move(m, si);
            nodes += perft(b, depth - 1);
            b.undo_move(m);
        }

        return nodes;
    };

    // Test 1: Starting position
    {
        Board board;
        const U64 expected[] = {1, 20, 400, 8902, 197281, 4865609};

        std::cout << "Starting position perft:\n";
        for (int depth = 1; depth <= 5; ++depth) {
            auto start = std::chrono::high_resolution_clock::now();
            U64 nodes = perft(board, depth);
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Depth " << depth << ": " << nodes;
            if (nodes == expected[depth]) {
                std::cout << " ✓";
            } else {
                std::cout << " ✗ (expected " << expected[depth] << ")";
            }
            std::cout << " (" << duration << " ms)\n";
        }
    }

    // Test 2: Kiwipete (many special moves)
    {
        Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        const U64 expected[] = {1, 48, 2039, 97862, 4085603};

        std::cout << "\nKiwipete position perft:\n";
        for (int depth = 1; depth <= 4; ++depth) {
            auto start = std::chrono::high_resolution_clock::now();
            U64 nodes = perft(board, depth);
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Depth " << depth << ": " << nodes;
            if (nodes == expected[depth]) {
                std::cout << " ✓";
            } else {
                std::cout << " ✗ (expected " << expected[depth] << ")";
            }
            std::cout << " (" << duration << " ms)\n";
        }
    }

    // Test 3: Position with en passant
    {
        Board board("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
        // At depth 1, should have 31 moves (including en passant)
        MoveList moves;
        MoveGen::generate_all(board, moves);
        int legal = 0;
        for (int i = 0; i < moves.size(); ++i) {
            if (MoveGen::is_legal(board, moves[i].move)) ++legal;
        }
        std::cout << "\nEn passant position: " << legal << " legal moves\n";
    }

    std::cout << std::endl;
}

// ============================================================================
// Main Entry Point
// ============================================================================

void run_tests() {
    std::cout << "===================================\n";
    std::cout << "  Chess Engine Test Suite\n";
    std::cout << "===================================\n\n";

    // Run Stage 1 tests (quick)
    test_bitboards();
    test_magic_bitboards();

    // Run Stage 2 tests
    test_move_generation();
    test_transposition_table();
    test_see();
    test_move_ordering();
    test_evaluation();
    test_perft();
    test_search();

    std::cout << "===================================\n";
    std::cout << "  All Tests Completed!\n";
    std::cout << "===================================\n";
}

void run_benchmark() {
    std::cout << "Running benchmark...\n\n";

    // Benchmark positions
    const char* positions[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"
    };

    const int numPositions = sizeof(positions) / sizeof(positions[0]);
    U64 totalNodes = 0;
    auto startTotal = std::chrono::steady_clock::now();

    for (int i = 0; i < numPositions; ++i) {
        Board board(positions[i]);

        SearchLimits limits;
        limits.depth = 10;

        std::cout << "Position " << (i + 1) << "/" << numPositions << ":\n";
        Searcher.start(board, limits);

        totalNodes += Searcher.stats().nodes;
        std::cout << std::endl;
    }

    auto endTotal = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count();

    std::cout << "===================================\n";
    std::cout << "Total nodes: " << totalNodes << "\n";
    std::cout << "Total time: " << elapsed << " ms\n";
    std::cout << "NPS: " << (elapsed > 0 ? totalNodes * 1000 / elapsed : totalNodes) << "\n";
    std::cout << "===================================\n";
}

void print_help() {
    std::cout << "Chess Engine - Command Line Options:\n";
    std::cout << "====================================\n\n";
    std::cout << "  (no args)       - Run in UCI mode\n";
    std::cout << "  test            - Run legacy test suite\n";
    std::cout << "  test perft [d]  - Run perft tests (optional depth)\n";
    std::cout << "  test tactical   - Run tactical puzzle suite\n";
    std::cout << "  test all        - Run complete test suite\n";
    std::cout << "  bench [d]       - Run benchmark (optional depth)\n";
    std::cout << "  bench time [ms] - Run time-based benchmark\n";
    std::cout << "  help            - Show this help message\n";
    std::cout << std::endl;
}

// File: src/main.cpp

int main(int argc, char* argv[]) {
    // Agar output lancar (sudah benar)
    std::cout.setf(std::ios::unitbuf);

    // 1. Inisialisasi Tabel Bitboard Dasar (Pion, Kuda, Raja)
    Bitboards::init();

    // 2. Inisialisasi Magic Bitboards (Gajah, Benteng, Menteri)
    // Jika ini terlewat, engine CRASH saat generate langkah sliding piece.
    Magics::init();

    // 3. Inisialisasi Zobrist Hashing
    // Jika ini terlewat, Hash Key 0 dan TT akan error.
    Zobrist::init();

    // (Opsional) Jika Position::init() memang ada dan melakukan hal lain,
    // biarkan saja. Tapi 3 di atas wajib dipanggil manual jika Position::init
    // tidak memanggilnya.
    Position::init();

    // Default: UCI mode
    UCI::UCIHandler uci;
    uci.loop();

    return 0;
}
