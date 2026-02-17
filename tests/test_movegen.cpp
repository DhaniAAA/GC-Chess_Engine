// ============================================================================
// test_movegen.cpp - Unit Tests for Move Generation (movegen.hpp/cpp)
// ============================================================================

// Test framework macros defined in test_main.cpp
#include "../include/movegen.hpp"
#include "../include/board.hpp"

// ============================================================================
// Helper function to count moves
// ============================================================================

int countMoves(const Board& board, void (*generator)(const Board&, MoveList&)) {
    MoveList moves;
    generator(board, moves);
    return moves.size();
}

int countLegalMoves(const Board& board) {
    MoveList moves;
    MoveGen::generate_legal(board, moves);
    return moves.size();
}

// ============================================================================
// Perft function for testing
// ============================================================================

U64 perft(Board& board, int depth) {
    if (depth == 0) return 1;

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    if (depth == 1) return moves.size();

    U64 nodes = 0;
    StateInfo si;

    for (auto& sm : moves) {
        board.do_move(sm.move, si);
        nodes += perft(board, depth - 1);
        board.undo_move(sm.move);
    }

    return nodes;
}

// ============================================================================
// Basic Move Generation Tests
// ============================================================================

TEST_CASE("Move generation - starting position", "[movegen][basic]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    REQUIRE(countLegalMoves(board) == 20);
}

TEST_CASE("Move generation - after 1.e4", "[movegen][basic]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", &si);

    REQUIRE(countLegalMoves(board) == 20);
}

// ============================================================================
// Pawn Move Tests
// ============================================================================

TEST_CASE("Pawn single push", "[movegen][pawn]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/8/4P3/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    bool foundE4 = false;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E3 && sm.move.to() == SQ_E4) {
            foundE4 = true;
            break;
        }
    }
    REQUIRE(foundE4);
}

TEST_CASE("Pawn double push", "[movegen][pawn]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/8/8/4P3/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    bool foundE3 = false;
    bool foundE4 = false;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E2) {
            if (sm.move.to() == SQ_E3) foundE3 = true;
            if (sm.move.to() == SQ_E4) foundE4 = true;
        }
    }
    REQUIRE(foundE3);
    REQUIRE(foundE4);
}

TEST_CASE("Pawn captures", "[movegen][pawn]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/3p4/4P3/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_captures(board, moves);

    bool foundCapture = false;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4 && sm.move.to() == SQ_D5) {
            foundCapture = true;
            break;
        }
    }
    REQUIRE(foundCapture);
}

TEST_CASE("Pawn promotions", "[movegen][pawn]") {
    Board board;
    StateInfo si;
    board.set("8/4P3/8/8/8/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int promoCount = 0;
    bool hasQueen = false, hasRook = false, hasBishop = false, hasKnight = false;

    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E7 && sm.move.to() == SQ_E8 && sm.move.is_promotion()) {
            promoCount++;
            switch (sm.move.promotion_type()) {
                case QUEEN: hasQueen = true; break;
                case ROOK: hasRook = true; break;
                case BISHOP: hasBishop = true; break;
                case KNIGHT: hasKnight = true; break;
                default: break;
            }
        }
    }

    REQUIRE(promoCount == 4);
    REQUIRE(hasQueen);
    REQUIRE(hasRook);
    REQUIRE(hasBishop);
    REQUIRE(hasKnight);
}

TEST_CASE("En passant capture", "[movegen][pawn]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/4Pp2/8/8/8/4K2k w - f6 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    bool foundEP = false;
    for (auto& sm : moves) {
        if (sm.move.is_enpassant() && sm.move.from() == SQ_E5 && sm.move.to() == SQ_F6) {
            foundEP = true;
            break;
        }
    }
    REQUIRE(foundEP);
}

// ============================================================================
// Knight Move Tests
// ============================================================================

TEST_CASE("Knight moves from center", "[movegen][knight]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/4N3/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int knightMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4) {
            knightMoves++;
        }
    }
    REQUIRE(knightMoves == 8);
}

TEST_CASE("Knight moves from corner", "[movegen][knight]") {
    Board board;
    StateInfo si;
    board.set("N7/8/8/8/8/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int knightMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_A8) {
            knightMoves++;
        }
    }
    REQUIRE(knightMoves == 2);
}

// ============================================================================
// Bishop Move Tests
// ============================================================================

TEST_CASE("Bishop moves on empty board", "[movegen][bishop]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/4B3/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int bishopMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4) {
            bishopMoves++;
        }
    }
    REQUIRE(bishopMoves == 13); // Diagonals from e4
}

// ============================================================================
// Rook Move Tests
// ============================================================================

TEST_CASE("Rook moves on empty board", "[movegen][rook]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/4R3/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int rookMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4) {
            rookMoves++;
        }
    }
    REQUIRE(rookMoves == 13); // Rank + file from e4 (blocked by King at e1)
}

// ============================================================================
// Queen Move Tests
// ============================================================================

TEST_CASE("Queen moves on empty board", "[movegen][queen]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/4Q3/8/8/4K2k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_all(board, moves);

    int queenMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4) {
            queenMoves++;
        }
    }
    REQUIRE(queenMoves == 26); // Rook (13) + Bishop (13)
}

// ============================================================================
// King Move Tests
// ============================================================================

TEST_CASE("King moves from center", "[movegen][king]") {
    Board board;
    StateInfo si;
    board.set("8/8/8/8/4K3/8/8/7k w - - 0 1", &si);

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    int kingMoves = 0;
    for (auto& sm : moves) {
        if (sm.move.from() == SQ_E4) {
            kingMoves++;
        }
    }
    REQUIRE(kingMoves == 8);
}

// ============================================================================
// Castling Tests
// ============================================================================

TEST_CASE("Castling moves - white", "[movegen][castling]") {
    Board board;
    StateInfo si;
    board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1", &si);

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    bool foundKingside = false;
    bool foundQueenside = false;

    for (auto& sm : moves) {
        if (sm.move.is_castling()) {
            if (sm.move.to() == SQ_G1) foundKingside = true;
            if (sm.move.to() == SQ_C1) foundQueenside = true;
        }
    }

    REQUIRE(foundKingside);
    REQUIRE(foundQueenside);
}

TEST_CASE("Castling blocked by pieces", "[movegen][castling]") {
    Board board;
    StateInfo si;
    board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/RN2K1NR w KQkq - 0 1", &si);

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    bool foundCastling = false;
    for (auto& sm : moves) {
        if (sm.move.is_castling()) {
            foundCastling = true;
            break;
        }
    }

    REQUIRE_FALSE(foundCastling);
}

TEST_CASE("Castling out of check is illegal", "[movegen][castling]") {
    Board board;
    StateInfo si;
    // King is in check from rook
    board.set("4r3/8/8/8/8/8/8/R3K2R w KQ - 0 1", &si);

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    bool foundCastling = false;
    for (auto& sm : moves) {
        if (sm.move.is_castling()) {
            foundCastling = true;
            break;
        }
    }

    REQUIRE_FALSE(foundCastling);
}

TEST_CASE("Castling through check is illegal", "[movegen][castling]") {
    Board board;
    StateInfo si;
    // f1 is attacked
    board.set("5r2/8/8/8/8/8/8/R3K2R w KQ - 0 1", &si);

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    bool foundKingsideCastling = false;
    for (auto& sm : moves) {
        if (sm.move.is_castling() && sm.move.to() == SQ_G1) {
            foundKingsideCastling = true;
            break;
        }
    }

    REQUIRE_FALSE(foundKingsideCastling);
}

// ============================================================================
// Check Evasion Tests
// ============================================================================

TEST_CASE("Check evasions", "[movegen][evasion]") {
    Board board;
    StateInfo si;
    // King in check from queen
    board.set("4k3/8/8/8/8/8/4q3/4K3 w - - 0 1", &si);

    REQUIRE(board.in_check());

    MoveList moves;
    MoveGen::generate_legal(board, moves);

    // All legal moves must be evasions
    REQUIRE(moves.size() > 0);

    for (auto& sm : moves) {
        StateInfo newSi;
        Board testBoard = board;
        testBoard.do_move(sm.move, newSi);
        REQUIRE_FALSE(testBoard.in_check());
    }
}

// ============================================================================
// Legality Tests
// ============================================================================

TEST_CASE("is_legal filters illegal moves", "[movegen][legal]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    MoveList allMoves;
    MoveGen::generate_all(board, allMoves);

    for (auto& sm : allMoves) {
        REQUIRE(MoveGen::is_legal(board, sm.move));
    }
}

TEST_CASE("is_pseudo_legal", "[movegen][pseudolegal]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    Move valid = Move::make(SQ_E2, SQ_E4);
    Move invalid = Move::make(SQ_E2, SQ_E5); // Not a legal pawn move

    REQUIRE(MoveGen::is_pseudo_legal(board, valid));
}

TEST_CASE("gives_check detection", "[movegen][check]") {
    Board board;
    StateInfo si;
    board.set("4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1", &si);

    Move checkMove = Move::make(SQ_E2, SQ_E7);
    REQUIRE(MoveGen::gives_check(board, checkMove));

    Move noCheckMove = Move::make(SQ_E2, SQ_A2);
    REQUIRE_FALSE(MoveGen::gives_check(board, noCheckMove));
}

// ============================================================================
// Perft Tests
// ============================================================================

TEST_CASE("Perft - starting position depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    REQUIRE(perft(board, 1) == 20);
}

TEST_CASE("Perft - starting position depth 2", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    REQUIRE(perft(board, 2) == 400);
}

TEST_CASE("Perft - starting position depth 3", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    REQUIRE(perft(board, 3) == 8902);
}

TEST_CASE("Perft - starting position depth 4", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    REQUIRE(perft(board, 4) == 197281);
}

TEST_CASE("Perft - Kiwipete position depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", &si);

    REQUIRE(perft(board, 1) == 48);
}

TEST_CASE("Perft - Kiwipete position depth 2", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", &si);

    REQUIRE(perft(board, 2) == 2039);
}

TEST_CASE("Perft - Position 3 depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", &si);

    REQUIRE(perft(board, 1) == 14);
}

TEST_CASE("Perft - Position 4 depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", &si);

    REQUIRE(perft(board, 1) == 6);
}

TEST_CASE("Perft - Position 5 depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", &si);

    REQUIRE(perft(board, 1) == 44);
}

TEST_CASE("Perft - Position 6 depth 1", "[movegen][perft]") {
    Board board;
    StateInfo si;
    board.set("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", &si);

    REQUIRE(perft(board, 1) == 46);
}
