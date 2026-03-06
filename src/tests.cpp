// ============================================================================
// tests.cpp - Test Suite Implementation
// ============================================================================
// This file contains all test functions for the chess engine.
// It is excluded from normal builds but can be included for testing.
// ============================================================================

#include "tests.hpp"
#include "board.hpp"
#include "bitboard.hpp"
#include "magic.hpp"
#include "zobrist.hpp"
#include "movegen.hpp"
#include "tt.hpp"
#include "moveorder.hpp"
#include "search.hpp"
#include "eval.hpp"
#include <iostream>
#include <chrono>
#include <functional>
#include <fstream>
#include <sstream>
#include <vector>
#include <iomanip>

namespace Tests {

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
    Move ttMoves[3] = {MOVE_NONE, MOVE_NONE, MOVE_NONE};
    MovePicker mp(board, ttMoves, 0, 0, kt, cm, ht, MOVE_NONE,
                  nullptr, nullptr, nullptr);

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

    ::Searcher.start(board, limits);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\nSearch complete!\n";
    std::cout << "Best move: " << move_to_string(::Searcher.best_move()) << "\n";
    std::cout << "Nodes: " << ::Searcher.stats().nodes << "\n";
    std::cout << "Time: " << duration << " ms\n";
    std::cout << "NPS: " << (::Searcher.stats().nodes * 1000 / (duration + 1)) << "\n\n";

    // Test a tactical position (mate in 2)
    Board mateIn2("r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4");
    std::cout << "Scholar's Mate position:\n";
    std::cout << mateIn2.pretty();

    limits.depth = 4;
    std::cout << "Searching for mate...\n\n";

    ::Searcher.start(mateIn2, limits);

    std::cout << "\nBest move: " << move_to_string(::Searcher.best_move()) << " (expected Qxf7#)\n\n";
}

void test_perft() {
    run_perft(5);
}

void run_perft(int maxDepth) {
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
        for (int depth = 1; depth <= std::min(maxDepth, 5); ++depth) {
            auto start = std::chrono::high_resolution_clock::now();
            U64 nodes = perft(board, depth);
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Depth " << depth << ": " << nodes;
            if (nodes == expected[depth]) {
                std::cout << " OK";
            } else {
                std::cout << " FAIL (expected " << expected[depth] << ")";
            }
            std::cout << " (" << duration << " ms)\n";
        }
    }

    // Test 2: Kiwipete (many special moves)
    {
        Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        const U64 expected[] = {1, 48, 2039, 97862, 4085603};

        std::cout << "\nKiwipete position perft:\n";
        for (int depth = 1; depth <= std::min(maxDepth, 4); ++depth) {
            auto start = std::chrono::high_resolution_clock::now();
            U64 nodes = perft(board, depth);
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Depth " << depth << ": " << nodes;
            if (nodes == expected[depth]) {
                std::cout << " OK";
            } else {
                std::cout << " FAIL (expected " << expected[depth] << ")";
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
// Test Runners
// ============================================================================

void run_all_tests() {
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
        ::Searcher.start(board, limits);

        totalNodes += ::Searcher.stats().nodes;
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

// ============================================================================
// WAC Test Suite Implementation
// ============================================================================

struct WACPosition {
    std::string fen;
    std::vector<std::string> bestMoves;  // One or more acceptable best moves
    std::string id;
};

// Parse a single EPD line
static bool parse_epd_line(const std::string& line, WACPosition& pos) {
    if (line.empty() || line[0] == '#') return false;

    // Find FEN parts (first 6 space-separated fields, but EPD may have only 4-6)
    std::istringstream iss(line);
    std::string token;
    std::string fen;

    // Position
    if (!(iss >> token)) return false;
    fen = token;

    // Side to move
    if (!(iss >> token)) return false;
    fen += " " + token;

    // Castling
    if (!(iss >> token)) return false;
    fen += " " + token;

    // En passant
    if (!(iss >> token)) return false;
    fen += " " + token;

    // EPD might not have halfmove/fullmove clocks, default to "0 1"
    fen += " 0 1";

    pos.fen = fen;

    // Parse operations (bm = best move, id = position id)
    std::string remaining;
    std::getline(iss, remaining);

    // Find bm (best move)
    size_t bmPos = remaining.find("bm ");
    if (bmPos != std::string::npos) {
        size_t start = bmPos + 3;
        size_t end = remaining.find(';', start);
        if (end == std::string::npos) end = remaining.length();

        std::string bmStr = remaining.substr(start, end - start);

        // Parse multiple best moves (space separated)
        std::istringstream bmIss(bmStr);
        std::string move;
        while (bmIss >> move) {
            // Remove trailing punctuation if any
            while (!move.empty() && (move.back() == ';' || move.back() == ',')) {
                move.pop_back();
            }
            if (!move.empty()) {
                pos.bestMoves.push_back(move);
            }
        }
    }

    // Find id
    size_t idPos = remaining.find("id \"");
    if (idPos != std::string::npos) {
        size_t start = idPos + 4;
        size_t end = remaining.find('"', start);
        if (end != std::string::npos) {
            pos.id = remaining.substr(start, end - start);
        }
    }

    return !pos.bestMoves.empty();
}

// Load WAC positions from file
static std::vector<WACPosition> load_wac_file(const std::string& filename) {
    std::vector<WACPosition> positions;
    std::ifstream file(filename);

    if (!file.is_open()) {
        return positions;  // Silently return empty — caller tries multiple paths
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        WACPosition pos;
        if (parse_epd_line(line, pos)) {
            positions.push_back(pos);
        }
    }

    return positions;
}

// Convert algebraic move to UCI format
static std::string algebraic_to_uci(const Board& board, const std::string& san) {
    MoveList moves;
    MoveGen::generate_legal(board, moves);

    // Normalize SAN for matching
    std::string normalized = san;
    // Remove check/mate indicators
    while (!normalized.empty() && (normalized.back() == '+' || normalized.back() == '#')) {
        normalized.pop_back();
    }

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        std::string uci = move_to_string(m);

        // Generate SAN for this move
        Piece pc = board.piece_on(m.from());
        PieceType pt = type_of(pc);
        Square from = m.from();
        Square to = m.to();

        std::string moveStr;

        if (m.is_castling()) {
            if (file_of(to) > file_of(from)) {
                moveStr = "O-O";
            } else {
                moveStr = "O-O-O";
            }
        } else {
            // Piece character
            if (pt != PAWN) {
                const char pieceChars[] = {' ', ' ', 'N', 'B', 'R', 'Q', 'K'};
                moveStr += pieceChars[pt];
            }

            // For disambiguation, check if other pieces of same type can reach target
            bool needFile = false, needRank = false;
            for (int j = 0; j < moves.size(); ++j) {
                if (i == j) continue;
                Move other = moves[j].move;
                Piece otherPc = board.piece_on(other.from());
                if (type_of(otherPc) == pt && other.to() == to) {
                    if (file_of(other.from()) == file_of(from)) needRank = true;
                    else needFile = true;
                }
            }

            // For pawns, always include file on captures
            if (pt == PAWN && file_of(from) != file_of(to)) {
                needFile = true;
            }

            if (needFile) moveStr += char('a' + file_of(from));
            if (needRank) moveStr += char('1' + rank_of(from));

            // Capture
            if (board.piece_on(to) != NO_PIECE || m.is_enpassant()) {
                moveStr += 'x';
            }

            // Destination
            moveStr += char('a' + file_of(to));
            moveStr += char('1' + rank_of(to));

            // Promotion
            if (m.is_promotion()) {
                const char promoChars[] = {' ', ' ', 'N', 'B', 'R', 'Q'};
                moveStr += '=';
                moveStr += promoChars[m.promotion_type()];
            }
        }

        // Match against input
        if (moveStr == normalized) {
            return uci;
        }

        // Also try matching without disambiguation
        std::string simpleStr;
        if (m.is_castling()) {
            simpleStr = moveStr;
        } else if (pt != PAWN) {
            const char pieceChars[] = {' ', ' ', 'N', 'B', 'R', 'Q', 'K'};
            simpleStr += pieceChars[pt];
            if (board.piece_on(to) != NO_PIECE) simpleStr += 'x';
            simpleStr += char('a' + file_of(to));
            simpleStr += char('1' + rank_of(to));
            if (m.is_promotion()) {
                const char promoChars[] = {' ', ' ', 'N', 'B', 'R', 'Q'};
                simpleStr += '=';
                simpleStr += promoChars[m.promotion_type()];
            }
        } else {
            if (file_of(from) != file_of(to)) {
                simpleStr += char('a' + file_of(from));
                simpleStr += 'x';
            }
            simpleStr += char('a' + file_of(to));
            simpleStr += char('1' + rank_of(to));
            if (m.is_promotion()) {
                const char promoChars[] = {' ', ' ', 'N', 'B', 'R', 'Q'};
                simpleStr += '=';
                simpleStr += promoChars[m.promotion_type()];
            }
        }

        if (simpleStr == normalized) {
            return uci;
        }
    }

    return "";
}

WACResult run_wac_test(int depth, int timeLimitMs, int startPos, int endPos, bool verbose) {
    WACResult result;

    // Try to find WAC file
    std::vector<std::string> paths = {
        "tests/wac.epd",
        "../tests/wac.epd",
        "wac.epd",
        "test/wac.epd"
    };

    std::vector<WACPosition> positions;
    for (const auto& path : paths) {
        positions = load_wac_file(path);
        if (!positions.empty()) {
            if (verbose) {
                std::cout << "Loaded " << positions.size() << " positions from " << path << std::endl;
            }
            break;
        }
    }

    if (positions.empty()) {
        std::cerr << "Error: Could not load WAC test suite" << std::endl;
        return result;
    }

    ::Searcher.set_silent(true);

    // Determine range
    int start = std::max(1, startPos) - 1;  // Convert to 0-based
    int end = (endPos <= 0 || endPos > (int)positions.size()) ? (int)positions.size() : endPos;

    std::cout << "\n========================================" << std::endl;
    std::cout << "  WAC Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Positions: " << (start + 1) << " - " << end << " of " << positions.size() << std::endl;
    if (depth > 0) {
        std::cout << "Depth: " << depth << std::endl;
    } else if (timeLimitMs > 0) {
        std::cout << "Time limit: " << timeLimitMs << " ms per position" << std::endl;
    }
    std::cout << "========================================\n" << std::endl;

    // Show progress indicator in quiet mode
    if (!verbose) {
        std::cout << "Running..." << std::flush;
    }

    // Collect failed positions for summary
    struct FailedPos {
        std::string id;
        std::string expected;
        std::string got;
    };
    std::vector<FailedPos> failedPositions;

    auto totalStart = std::chrono::steady_clock::now();

    for (int i = start; i < end; ++i) {
        const WACPosition& pos = positions[i];
        result.total++;

        // Progress indicator for quiet mode
        if (!verbose) {
            int posNum = i - start + 1;
            int totalPositions = end - start;
            if (posNum % 50 == 0) {
                int pct = posNum * 100 / totalPositions;
                std::cout << " " << pct << "%" << std::flush;
            } else if (posNum % 10 == 0) {
                std::cout << "." << std::flush;
            }
        }

        // Set up position
        StateInfo si;
        Board board;
        try {
            board.set(pos.fen, &si);
        } catch (...) {
            if (verbose) {
                std::cout << pos.id << ": ERROR - Invalid FEN" << std::endl;
            }
            result.errors++;
            continue;
        }

        // Search
        SearchLimits limits;
        if (depth > 0) {
            limits.depth = depth;
        } else if (timeLimitMs > 0) {
            limits.movetime = timeLimitMs;
        } else {
            limits.depth = 10;  // Default
        }

        auto posStart = std::chrono::steady_clock::now();
        ::Searcher.start(board, limits);
        auto posEnd = std::chrono::steady_clock::now();

        auto posTime = std::chrono::duration_cast<std::chrono::milliseconds>(posEnd - posStart).count();
        result.totalTimeMs += posTime;
        result.totalNodes += ::Searcher.stats().nodes;

        Move bestMove = ::Searcher.best_move();
        std::string uciMove = move_to_string(bestMove);

        // Check if our move matches any of the expected best moves
        bool passed = false;
        for (const auto& expectedSAN : pos.bestMoves) {
            // Convert SAN to UCI for comparison
            std::string expectedUCI = algebraic_to_uci(board, expectedSAN);

            if (uciMove == expectedUCI) {
                passed = true;
                break;
            }

            // Also check if the raw UCI matches (in case EPD had UCI notation)
            if (uciMove == expectedSAN ||
                (uciMove.length() >= 4 && expectedSAN.length() >= 4 &&
                 uciMove.substr(0, 4) == expectedSAN.substr(0, 4))) {
                passed = true;
                break;
            }
        }

        if (passed) {
            result.passed++;
            if (verbose) {
                std::cout << pos.id << ": PASS - Found " << uciMove
                          << " (" << ::Searcher.stats().nodes << " nodes, "
                          << posTime << " ms)" << std::endl;
            }
        } else {
            result.failed++;
            // Collect failed position info
            std::string expectedStr;
            for (size_t j = 0; j < pos.bestMoves.size(); ++j) {
                if (j > 0) expectedStr += " or ";
                expectedStr += pos.bestMoves[j];
            }
            failedPositions.push_back({pos.id, expectedStr, uciMove});

            if (verbose) {
                std::cout << pos.id << ": FAIL - Expected " << expectedStr
                          << ", got " << uciMove << std::endl;
            }
        }
    }

    // End progress line in quiet mode
    if (!verbose) {
        std::cout << " Done!" << std::endl;
    }

    auto totalEnd = std::chrono::steady_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();

    ::Searcher.set_silent(false);

    // Print summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "  WAC Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Total: " << result.total << std::endl;
    std::cout << "Passed: " << result.passed << " (" << std::fixed << std::setprecision(1)
              << result.pass_rate() << "%)" << std::endl;
    std::cout << "Failed: " << result.failed << std::endl;
    if (result.errors > 0) {
        std::cout << "Errors: " << result.errors << std::endl;
    }
    std::cout << "Total Nodes: " << result.totalNodes << std::endl;
    std::cout << "Total Time: " << totalTime << " ms" << std::endl;
    if (totalTime > 0) {
        std::cout << "Avg NPS: " << (result.totalNodes * 1000 / totalTime) << std::endl;
    }
    std::cout << "========================================" << std::endl;

    // Print list of all failed positions
    if (!failedPositions.empty()) {
        std::cout << "\nFailed positions (" << failedPositions.size() << "):" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        for (const auto& fp : failedPositions) {
            std::cout << "  " << fp.id << ": expected " << fp.expected
                      << ", got " << fp.got << std::endl;
        }
        std::cout << "----------------------------------------" << std::endl;
    }
    std::cout << std::endl;

    return result;
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
    std::cout << "  wac [options]   - Run WAC test suite\n";
    std::cout << "  help            - Show this help message\n";
    std::cout << std::endl;
}

} // namespace Tests
