// ============================================================================
// test_board.cpp - Unit Tests for Board Representation (board.hpp/cpp)
// ============================================================================

// Test framework macros defined in test_main.cpp
#include "../include/board.hpp"

// Standard starting position FEN
const std::string STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// ============================================================================
// Construction & FEN Tests
// ============================================================================

TEST_CASE("Board default constructor creates starting position", "[board][construction]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    REQUIRE(board.side_to_move() == WHITE);
    REQUIRE(board.castling_rights() == ALL_CASTLING);
    REQUIRE(board.en_passant_square() == SQ_NONE);
    REQUIRE(board.halfmove_clock() == 0);
}

TEST_CASE("Board FEN parsing - starting position", "[board][fen]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    SECTION("White pieces on rank 1") {
        REQUIRE(board.piece_on(SQ_A1) == W_ROOK);
        REQUIRE(board.piece_on(SQ_B1) == W_KNIGHT);
        REQUIRE(board.piece_on(SQ_C1) == W_BISHOP);
        REQUIRE(board.piece_on(SQ_D1) == W_QUEEN);
        REQUIRE(board.piece_on(SQ_E1) == W_KING);
        REQUIRE(board.piece_on(SQ_F1) == W_BISHOP);
        REQUIRE(board.piece_on(SQ_G1) == W_KNIGHT);
        REQUIRE(board.piece_on(SQ_H1) == W_ROOK);
    }

    SECTION("White pawns on rank 2") {
        for (File f = FILE_A; f <= FILE_H; ++f) {
            REQUIRE(board.piece_on(make_square(f, RANK_2)) == W_PAWN);
        }
    }

    SECTION("Black pieces on rank 8") {
        REQUIRE(board.piece_on(SQ_A8) == B_ROOK);
        REQUIRE(board.piece_on(SQ_B8) == B_KNIGHT);
        REQUIRE(board.piece_on(SQ_C8) == B_BISHOP);
        REQUIRE(board.piece_on(SQ_D8) == B_QUEEN);
        REQUIRE(board.piece_on(SQ_E8) == B_KING);
        REQUIRE(board.piece_on(SQ_F8) == B_BISHOP);
        REQUIRE(board.piece_on(SQ_G8) == B_KNIGHT);
        REQUIRE(board.piece_on(SQ_H8) == B_ROOK);
    }

    SECTION("Black pawns on rank 7") {
        for (File f = FILE_A; f <= FILE_H; ++f) {
            REQUIRE(board.piece_on(make_square(f, RANK_7)) == B_PAWN);
        }
    }

    SECTION("Empty squares in middle") {
        for (Rank r = RANK_3; r <= RANK_6; ++r) {
            for (File f = FILE_A; f <= FILE_H; ++f) {
                REQUIRE(board.empty(make_square(f, r)));
            }
        }
    }
}

TEST_CASE("Board FEN parsing - complex positions", "[board][fen]") {
    Board board;
    StateInfo si;

    SECTION("After 1.e4") {
        board.set("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", &si);
        REQUIRE(board.side_to_move() == BLACK);
        REQUIRE(board.piece_on(SQ_E4) == W_PAWN);
        REQUIRE(board.empty(SQ_E2));
        REQUIRE(board.en_passant_square() == SQ_E3);
    }

    SECTION("Position with no castling rights") {
        board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w - - 0 1", &si);
        REQUIRE(board.castling_rights() == NO_CASTLING);
    }

    SECTION("Position with partial castling rights") {
        board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w Kq - 0 1", &si);
        REQUIRE((board.castling_rights() & WHITE_OO) != 0);
        REQUIRE((board.castling_rights() & WHITE_OOO) == 0);
        REQUIRE((board.castling_rights() & BLACK_OO) == 0);
        REQUIRE((board.castling_rights() & BLACK_OOO) != 0);
    }

    SECTION("Halfmove clock and game ply") {
        board.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 42 50", &si);
        REQUIRE(board.halfmove_clock() == 42);
    }
}

TEST_CASE("Board FEN generation", "[board][fen]") {
    Board board;
    StateInfo si;

    SECTION("Starting position round-trip") {
        board.set(STARTPOS, &si);
        std::string generated = board.fen();
        // Compare essential parts (ignore ply numbers)
        REQUIRE(generated.find("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -") == 0);
    }

    SECTION("Complex position round-trip") {
        std::string fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
        board.set(fen, &si);
        std::string generated = board.fen();
        REQUIRE(generated.find("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq") == 0);
    }
}

// ============================================================================
// Piece Access Tests
// ============================================================================

TEST_CASE("Board piece bitboards", "[board][pieces]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    SECTION("pieces() returns all occupied squares") {
        Bitboard all = board.pieces();
        REQUIRE(popcount(all) == 32);
    }

    SECTION("pieces(PieceType) returns correct pieces") {
        REQUIRE(popcount(board.pieces(PAWN)) == 16);
        REQUIRE(popcount(board.pieces(KNIGHT)) == 4);
        REQUIRE(popcount(board.pieces(BISHOP)) == 4);
        REQUIRE(popcount(board.pieces(ROOK)) == 4);
        REQUIRE(popcount(board.pieces(QUEEN)) == 2);
        REQUIRE(popcount(board.pieces(KING)) == 2);
    }

    SECTION("pieces(Color) returns correct color") {
        REQUIRE(popcount(board.pieces(WHITE)) == 16);
        REQUIRE(popcount(board.pieces(BLACK)) == 16);
    }

    SECTION("pieces(Color, PieceType) returns correct pieces") {
        REQUIRE(popcount(board.pieces(WHITE, PAWN)) == 8);
        REQUIRE(popcount(board.pieces(BLACK, PAWN)) == 8);
        REQUIRE(popcount(board.pieces(WHITE, KING)) == 1);
    }

    SECTION("pieces(Color, PieceType1, PieceType2) combines types") {
        Bitboard bishops_and_queens = board.pieces(WHITE, BISHOP, QUEEN);
        REQUIRE(popcount(bishops_and_queens) == 3); // 2 bishops + 1 queen
    }
}

TEST_CASE("Board king square", "[board][pieces]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    REQUIRE(board.king_square(WHITE) == SQ_E1);
    REQUIRE(board.king_square(BLACK) == SQ_E8);
}

TEST_CASE("Board piece count", "[board][pieces]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    REQUIRE(board.count(WHITE, PAWN) == 8);
    REQUIRE(board.count(WHITE, KNIGHT) == 2);
    REQUIRE(board.count(WHITE, BISHOP) == 2);
    REQUIRE(board.count(WHITE, ROOK) == 2);
    REQUIRE(board.count(WHITE, QUEEN) == 1);
    REQUIRE(board.count(WHITE, KING) == 1);
}

// ============================================================================
// Attack Detection Tests
// ============================================================================

TEST_CASE("Board attackers_to", "[board][attacks]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    SECTION("E4 is not attacked in starting position") {
        Bitboard attackers = board.attackers_to(SQ_E4);
        REQUIRE(popcount(attackers) == 0);
    }

    SECTION("E3 attacked by white pawns") {
        Bitboard attackers = board.attackers_to(SQ_E3);
        REQUIRE((attackers & board.pieces(WHITE, PAWN)) != 0);
    }
}

TEST_CASE("Board is_attacked_by", "[board][attacks]") {
    Board board;
    StateInfo si;
    board.set(STARTPOS, &si);

    REQUIRE_FALSE(board.is_attacked_by(WHITE, SQ_E4));
    REQUIRE_FALSE(board.is_attacked_by(BLACK, SQ_E4));
}

TEST_CASE("Board in_check and checkers", "[board][attacks]") {
    Board board;
    StateInfo si;

    SECTION("Starting position - no check") {
        board.set(STARTPOS, &si);
        REQUIRE_FALSE(board.in_check());
        REQUIRE(board.checkers() == 0);
    }

    SECTION("Position with check") {
        // Scholar's mate position - white queen gives check
        board.set("r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4", &si);
        REQUIRE(board.in_check());
        REQUIRE(board.checkers() != 0);
    }
}

// ============================================================================
// Move Making Tests
// ============================================================================

TEST_CASE("Board do_move - quiet moves", "[board][domove]") {
    Board board;
    StateInfo si, newSi;
    // Setup board with black pawn at d4 to validate En Passant (Strict EP rule)
    board.set("rnbqkbnr/pppppppp/8/8/3p4/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &si);

    Move e2e4 = Move::make(SQ_E2, SQ_E4);
    board.do_move(e2e4, newSi);

    REQUIRE(board.piece_on(SQ_E4) == W_PAWN);
    REQUIRE(board.empty(SQ_E2));
    REQUIRE(board.side_to_move() == BLACK);
    REQUIRE(board.en_passant_square() == SQ_E3);
}

TEST_CASE("Board do_move - captures", "[board][domove]") {
    Board board;
    StateInfo si, newSi;
    // Position where white knight can capture black pawn
    board.set("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3", &si);

    Move nxe5 = Move::make(SQ_F3, SQ_E5);
    Piece captured = board.piece_on(SQ_E5);
    REQUIRE(captured == B_PAWN);

    board.do_move(nxe5, newSi);

    REQUIRE(board.piece_on(SQ_E5) == W_KNIGHT);
    REQUIRE(board.empty(SQ_F3));
}

TEST_CASE("Board do_move - castling", "[board][domove]") {
    Board board;
    StateInfo si, newSi;
    // Position where white can castle kingside
    // Also add black pawn at d4 for EP tests if needed later
    board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1", &si);

    SECTION("Kingside castling") {
        board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1", &si);
        Move castle = Move::make_castling(SQ_E1, SQ_G1);
        board.do_move(castle, newSi);

        REQUIRE(board.piece_on(SQ_G1) == W_KING);
        REQUIRE(board.piece_on(SQ_F1) == W_ROOK);
        REQUIRE(board.empty(SQ_E1));
        REQUIRE(board.empty(SQ_H1));
        REQUIRE((board.castling_rights() & WHITE_CASTLING) == 0);
    }

    SECTION("Queenside castling") {
        board.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1", &si);
        Move castle = Move::make_castling(SQ_E1, SQ_C1);
        board.do_move(castle, newSi);

        REQUIRE(board.piece_on(SQ_C1) == W_KING);
        REQUIRE(board.piece_on(SQ_D1) == W_ROOK);
        REQUIRE(board.empty(SQ_E1));
        REQUIRE(board.empty(SQ_A1));
    }
}

TEST_CASE("Board do_move - promotion", "[board][domove]") {
    Board board;
    StateInfo si, newSi;
    board.set("8/P7/8/8/8/8/8/4K2k w - - 0 1", &si);

    Move promo = Move::make_promotion(SQ_A7, SQ_A8, QUEEN);
    board.do_move(promo, newSi);

    REQUIRE(board.piece_on(SQ_A8) == W_QUEEN);
    REQUIRE(board.empty(SQ_A7));
}

TEST_CASE("Board do_move - en passant", "[board][domove]") {
    Board board;
    StateInfo si, newSi;
    board.set("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 3", &si);

    Move ep = Move::make_enpassant(SQ_F5, SQ_E6);
    board.do_move(ep, newSi);

    REQUIRE(board.piece_on(SQ_E6) == W_PAWN);
    REQUIRE(board.empty(SQ_F5));
    REQUIRE(board.empty(SQ_E5)); // Captured pawn removed
}

TEST_CASE("Board undo_move", "[board][undomove]") {
    Board board;
    StateInfo si, newSi;
    board.set(STARTPOS, &si);

    // Store original state
    Key originalKey = board.key();
    Color originalSide = board.side_to_move();

    Move e2e4 = Move::make(SQ_E2, SQ_E4);
    board.do_move(e2e4, newSi);
    board.undo_move(e2e4);

    REQUIRE(board.piece_on(SQ_E2) == W_PAWN);
    REQUIRE(board.empty(SQ_E4));
    REQUIRE(board.side_to_move() == originalSide);
    REQUIRE(board.key() == originalKey);
}

TEST_CASE("Board null move", "[board][nullmove]") {
    Board board;
    StateInfo si, newSi;
    board.set(STARTPOS, &si);

    Color originalSide = board.side_to_move();
    board.do_null_move(newSi);

    REQUIRE(board.side_to_move() == ~originalSide);
    REQUIRE(board.en_passant_square() == SQ_NONE);

    board.undo_null_move();
    REQUIRE(board.side_to_move() == originalSide);
}

// ============================================================================
// Key Computation Tests
// ============================================================================

TEST_CASE("Board key computation", "[board][key]") {
    Board board1, board2;
    StateInfo si1, si2;

    SECTION("Same position has same key") {
        board1.set(STARTPOS, &si1);
        board2.set(STARTPOS, &si2);
        REQUIRE(board1.key() == board2.key());
    }

    SECTION("Different positions have different keys") {
        board1.set(STARTPOS, &si1);
        board2.set("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", &si2);
        REQUIRE(board1.key() != board2.key());
    }
}

TEST_CASE("Board incremental key update", "[board][key]") {
    Board board;
    StateInfo si, newSi;
    board.set(STARTPOS, &si);

    Key originalKey = board.key();

    Move e2e4 = Move::make(SQ_E2, SQ_E4);
    board.do_move(e2e4, newSi);

    Key afterMoveKey = board.key();
    REQUIRE(afterMoveKey != originalKey); // Key should change after move

    board.undo_move(e2e4);
    Key afterUndoKey = board.key();
    REQUIRE(afterUndoKey == originalKey); // Key should be restored after undo
}

// ============================================================================
// Validation Tests
// ============================================================================

TEST_CASE("Board is_valid", "[board][validation]") {
    Board board;
    StateInfo si;

    SECTION("Starting position is valid") {
        board.set(STARTPOS, &si);
        REQUIRE(board.is_valid());
    }
}

TEST_CASE("Board is_capture", "[board][capture]") {
    Board board;
    StateInfo si;
    board.set("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3", &si);

    Move capture = Move::make(SQ_F3, SQ_E5);
    Move quiet = Move::make(SQ_F3, SQ_G5);

    REQUIRE(board.is_capture(capture));
    REQUIRE_FALSE(board.is_capture(quiet));
}
