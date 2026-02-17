// ============================================================================
// test_types.cpp - Unit Tests for Basic Types (types.hpp)
// ============================================================================

// Test framework macros defined in test_main.cpp
#include "../include/types.hpp"

// ============================================================================
// Color Tests
// ============================================================================

TEST_CASE("Color flip operator", "[types][color]") {
    SECTION("WHITE flips to BLACK") {
        REQUIRE(~WHITE == BLACK);
    }

    SECTION("BLACK flips to WHITE") {
        REQUIRE(~BLACK == WHITE);
    }

    SECTION("Double flip returns original") {
        REQUIRE(~~WHITE == WHITE);
        REQUIRE(~~BLACK == BLACK);
    }
}

// ============================================================================
// Square Tests
// ============================================================================

TEST_CASE("make_square creates correct squares", "[types][square]") {
    REQUIRE(make_square(FILE_A, RANK_1) == SQ_A1);
    REQUIRE(make_square(FILE_E, RANK_4) == SQ_E4);
    REQUIRE(make_square(FILE_H, RANK_8) == SQ_H8);
    REQUIRE(make_square(FILE_D, RANK_5) == SQ_D5);
}

TEST_CASE("file_of extracts correct file", "[types][square]") {
    REQUIRE(file_of(SQ_A1) == FILE_A);
    REQUIRE(file_of(SQ_E4) == FILE_E);
    REQUIRE(file_of(SQ_H8) == FILE_H);
    REQUIRE(file_of(SQ_D5) == FILE_D);
}

TEST_CASE("rank_of extracts correct rank", "[types][square]") {
    REQUIRE(rank_of(SQ_A1) == RANK_1);
    REQUIRE(rank_of(SQ_E4) == RANK_4);
    REQUIRE(rank_of(SQ_H8) == RANK_8);
    REQUIRE(rank_of(SQ_D5) == RANK_5);
}

TEST_CASE("relative_rank works for both colors", "[types][square]") {
    SECTION("White perspective") {
        REQUIRE(relative_rank(WHITE, RANK_1) == RANK_1);
        REQUIRE(relative_rank(WHITE, RANK_7) == RANK_7);
        REQUIRE(relative_rank(WHITE, RANK_8) == RANK_8);
    }

    SECTION("Black perspective") {
        REQUIRE(relative_rank(BLACK, RANK_1) == RANK_8);
        REQUIRE(relative_rank(BLACK, RANK_7) == RANK_2);
        REQUIRE(relative_rank(BLACK, RANK_8) == RANK_1);
    }
}

TEST_CASE("relative_square works for both colors", "[types][square]") {
    SECTION("White perspective - no change") {
        REQUIRE(relative_square(WHITE, SQ_A1) == SQ_A1);
        REQUIRE(relative_square(WHITE, SQ_E4) == SQ_E4);
    }

    SECTION("Black perspective - flipped") {
        REQUIRE(relative_square(BLACK, SQ_A1) == SQ_A8);
        REQUIRE(relative_square(BLACK, SQ_A8) == SQ_A1);
        REQUIRE(relative_square(BLACK, SQ_E4) == SQ_E5);
    }
}

TEST_CASE("pawn_push returns correct direction", "[types][direction]") {
    REQUIRE(pawn_push(WHITE) == NORTH);
    REQUIRE(pawn_push(BLACK) == SOUTH);
}

TEST_CASE("is_valid_square validates square range", "[types][square]") {
    REQUIRE(is_valid_square(SQ_A1));
    REQUIRE(is_valid_square(SQ_H8));
    REQUIRE(is_valid_square(SQ_E4));
    REQUIRE_FALSE(is_valid_square(SQ_NONE));
    REQUIRE_FALSE(is_valid_square(Square(64)));
    REQUIRE_FALSE(is_valid_square(Square(-1)));
}

// ============================================================================
// Piece Tests
// ============================================================================

TEST_CASE("make_piece creates correct pieces", "[types][piece]") {
    REQUIRE(make_piece(WHITE, PAWN) == W_PAWN);
    REQUIRE(make_piece(WHITE, KNIGHT) == W_KNIGHT);
    REQUIRE(make_piece(WHITE, BISHOP) == W_BISHOP);
    REQUIRE(make_piece(WHITE, ROOK) == W_ROOK);
    REQUIRE(make_piece(WHITE, QUEEN) == W_QUEEN);
    REQUIRE(make_piece(WHITE, KING) == W_KING);

    REQUIRE(make_piece(BLACK, PAWN) == B_PAWN);
    REQUIRE(make_piece(BLACK, KNIGHT) == B_KNIGHT);
    REQUIRE(make_piece(BLACK, BISHOP) == B_BISHOP);
    REQUIRE(make_piece(BLACK, ROOK) == B_ROOK);
    REQUIRE(make_piece(BLACK, QUEEN) == B_QUEEN);
    REQUIRE(make_piece(BLACK, KING) == B_KING);
}

TEST_CASE("color_of extracts correct color", "[types][piece]") {
    REQUIRE(color_of(W_PAWN) == WHITE);
    REQUIRE(color_of(W_KING) == WHITE);
    REQUIRE(color_of(B_PAWN) == BLACK);
    REQUIRE(color_of(B_KING) == BLACK);
}

TEST_CASE("type_of extracts correct piece type", "[types][piece]") {
    REQUIRE(type_of(W_PAWN) == PAWN);
    REQUIRE(type_of(W_KNIGHT) == KNIGHT);
    REQUIRE(type_of(W_BISHOP) == BISHOP);
    REQUIRE(type_of(W_ROOK) == ROOK);
    REQUIRE(type_of(W_QUEEN) == QUEEN);
    REQUIRE(type_of(W_KING) == KING);

    REQUIRE(type_of(B_PAWN) == PAWN);
    REQUIRE(type_of(B_KNIGHT) == KNIGHT);
    REQUIRE(type_of(B_KING) == KING);
}

// ============================================================================
// Castling Rights Tests
// ============================================================================

TEST_CASE("CastlingRights OR operator", "[types][castling]") {
    REQUIRE((WHITE_OO | WHITE_OOO) == WHITE_CASTLING);
    REQUIRE((BLACK_OO | BLACK_OOO) == BLACK_CASTLING);
    REQUIRE((WHITE_CASTLING | BLACK_CASTLING) == ALL_CASTLING);
}

TEST_CASE("CastlingRights AND operator", "[types][castling]") {
    REQUIRE((ALL_CASTLING & WHITE_CASTLING) == WHITE_CASTLING);
    REQUIRE((ALL_CASTLING & BLACK_OO) == BLACK_OO);
    REQUIRE((WHITE_OO & BLACK_OO) == NO_CASTLING);
}

TEST_CASE("CastlingRights NOT operator", "[types][castling]") {
    CastlingRights cr = ALL_CASTLING;
    cr &= ~WHITE_OO;
    REQUIRE((cr & WHITE_OO) == NO_CASTLING);
    REQUIRE((cr & WHITE_OOO) == WHITE_OOO);
}

// ============================================================================
// EvalScore Tests
// ============================================================================

TEST_CASE("EvalScore default constructor", "[types][evalscore]") {
    EvalScore s;
    REQUIRE(s.mg == 0);
    REQUIRE(s.eg == 0);
}

TEST_CASE("EvalScore parameterized constructor", "[types][evalscore]") {
    EvalScore s(100, 50);
    REQUIRE(s.mg == 100);
    REQUIRE(s.eg == 50);
}

TEST_CASE("EvalScore addition", "[types][evalscore]") {
    EvalScore a(100, 50);
    EvalScore b(25, 75);
    EvalScore c = a + b;
    REQUIRE(c.mg == 125);
    REQUIRE(c.eg == 125);
}

TEST_CASE("EvalScore subtraction", "[types][evalscore]") {
    EvalScore a(100, 50);
    EvalScore b(25, 25);
    EvalScore c = a - b;
    REQUIRE(c.mg == 75);
    REQUIRE(c.eg == 25);
}

TEST_CASE("EvalScore negation", "[types][evalscore]") {
    EvalScore a(100, -50);
    EvalScore b = -a;
    REQUIRE(b.mg == -100);
    REQUIRE(b.eg == 50);
}

TEST_CASE("EvalScore scalar multiplication", "[types][evalscore]") {
    EvalScore a(10, 5);
    EvalScore b = a * 3;
    REQUIRE(b.mg == 30);
    REQUIRE(b.eg == 15);
}

TEST_CASE("EvalScore compound assignment", "[types][evalscore]") {
    SECTION("Addition assignment") {
        EvalScore a(100, 50);
        a += EvalScore(25, 25);
        REQUIRE(a.mg == 125);
        REQUIRE(a.eg == 75);
    }

    SECTION("Subtraction assignment") {
        EvalScore a(100, 50);
        a -= EvalScore(25, 25);
        REQUIRE(a.mg == 75);
        REQUIRE(a.eg == 25);
    }
}

TEST_CASE("S() shorthand creates EvalScore", "[types][evalscore]") {
    EvalScore s = S(100, 50);
    REQUIRE(s.mg == 100);
    REQUIRE(s.eg == 50);
}

// ============================================================================
// String Conversion Tests
// ============================================================================

TEST_CASE("square_to_string converts correctly", "[types][string]") {
    REQUIRE(square_to_string(SQ_A1) == "a1");
    REQUIRE(square_to_string(SQ_E4) == "e4");
    REQUIRE(square_to_string(SQ_H8) == "h8");
    REQUIRE(square_to_string(SQ_D5) == "d5");
}

TEST_CASE("string_to_square parses correctly", "[types][string]") {
    REQUIRE(string_to_square("a1") == SQ_A1);
    REQUIRE(string_to_square("e4") == SQ_E4);
    REQUIRE(string_to_square("h8") == SQ_H8);
    REQUIRE(string_to_square("d5") == SQ_D5);
}

TEST_CASE("string_to_square handles invalid input", "[types][string]") {
    REQUIRE(string_to_square("") == SQ_NONE);
    REQUIRE(string_to_square("x") == SQ_NONE);
    REQUIRE(string_to_square("i1") == SQ_NONE);
    REQUIRE(string_to_square("a9") == SQ_NONE);
}
