// ============================================================================
// test_bitboard.cpp - Unit Tests for Bitboard Operations (bitboard.hpp)
// ============================================================================

// Test framework macros defined in test_main.cpp
#include "../include/bitboard.hpp"

// ============================================================================
// Basic Bitboard Tests
// ============================================================================

TEST_CASE("square_bb creates correct bitboard", "[bitboard][basic]") {
    REQUIRE(square_bb(SQ_A1) == 1ULL);
    REQUIRE(square_bb(SQ_H1) == (1ULL << 7));
    REQUIRE(square_bb(SQ_A8) == (1ULL << 56));
    REQUIRE(square_bb(SQ_H8) == (1ULL << 63));
    REQUIRE(square_bb(SQ_E4) == (1ULL << 28));
}

TEST_CASE("Bitboard AND operator with Square", "[bitboard][operators]") {
    Bitboard b = 0x0000000000000001ULL; // Only A1
    REQUIRE((b & SQ_A1) != 0);
    REQUIRE((b & SQ_B1) == 0);
    REQUIRE((b & SQ_A2) == 0);
}

TEST_CASE("Bitboard OR operator with Square", "[bitboard][operators]") {
    Bitboard b = 0;
    b = b | SQ_A1;
    b = b | SQ_H8;
    REQUIRE((b & SQ_A1) != 0);
    REQUIRE((b & SQ_H8) != 0);
    REQUIRE((b & SQ_E4) == 0);
}

TEST_CASE("Bitboard XOR operator with Square", "[bitboard][operators]") {
    Bitboard b = square_bb(SQ_E4);
    b = b ^ SQ_E4;
    REQUIRE(b == 0);

    b = b ^ SQ_E4;
    REQUIRE(b == square_bb(SQ_E4));
}

TEST_CASE("more_than_one detects multiple bits", "[bitboard][basic]") {
    REQUIRE_FALSE(more_than_one(0));
    REQUIRE_FALSE(more_than_one(1ULL));
    REQUIRE_FALSE(more_than_one(square_bb(SQ_E4)));
    REQUIRE(more_than_one(square_bb(SQ_A1) | square_bb(SQ_H8)));
    REQUIRE(more_than_one(FULL_BB));
}

// ============================================================================
// Bit Manipulation Tests
// ============================================================================

TEST_CASE("popcount counts bits correctly", "[bitboard][popcount]") {
    REQUIRE(popcount(0) == 0);
    REQUIRE(popcount(1ULL) == 1);
    REQUIRE(popcount(0xFFULL) == 8);
    REQUIRE(popcount(FULL_BB) == 64);
    REQUIRE(popcount(FILE_A_BB) == 8);
    REQUIRE(popcount(RANK_1_BB) == 8);
}

TEST_CASE("lsb finds least significant bit", "[bitboard][lsb]") {
    REQUIRE(lsb(1ULL) == SQ_A1);
    REQUIRE(lsb(square_bb(SQ_E4)) == SQ_E4);
    REQUIRE(lsb(RANK_1_BB) == SQ_A1);
    REQUIRE(lsb(FILE_H_BB) == SQ_H1);
    REQUIRE(lsb(square_bb(SQ_H8)) == SQ_H8);
}

TEST_CASE("msb finds most significant bit", "[bitboard][msb]") {
    REQUIRE(msb(1ULL) == SQ_A1);
    REQUIRE(msb(square_bb(SQ_E4)) == SQ_E4);
    REQUIRE(msb(RANK_1_BB) == SQ_H1);
    REQUIRE(msb(FILE_A_BB) == SQ_A8);
    REQUIRE(msb(FULL_BB) == SQ_H8);
}

TEST_CASE("pop_lsb pops and returns lsb", "[bitboard][pop_lsb]") {
    Bitboard b = square_bb(SQ_A1) | square_bb(SQ_E4) | square_bb(SQ_H8);

    Square s1 = pop_lsb(b);
    REQUIRE(s1 == SQ_A1);
    REQUIRE(popcount(b) == 2);

    Square s2 = pop_lsb(b);
    REQUIRE(s2 == SQ_E4);
    REQUIRE(popcount(b) == 1);

    Square s3 = pop_lsb(b);
    REQUIRE(s3 == SQ_H8);
    REQUIRE(b == 0);
}

// ============================================================================
// Shift Tests
// ============================================================================

TEST_CASE("shift<NORTH> moves bits up", "[bitboard][shift]") {
    Bitboard b = RANK_1_BB;
    Bitboard shifted = shift<NORTH>(b);
    REQUIRE(shifted == RANK_2_BB);

    // Shifting rank 8 should give 0 (wrap around check)
    b = RANK_8_BB;
    shifted = shift<NORTH>(b);
    REQUIRE(shifted == 0);
}

TEST_CASE("shift<SOUTH> moves bits down", "[bitboard][shift]") {
    Bitboard b = RANK_8_BB;
    Bitboard shifted = shift<SOUTH>(b);
    REQUIRE(shifted == RANK_7_BB);

    // Shifting rank 1 should give 0
    b = RANK_1_BB;
    shifted = shift<SOUTH>(b);
    REQUIRE(shifted == 0);
}

TEST_CASE("shift<EAST> moves bits right without wrap", "[bitboard][shift]") {
    Bitboard b = FILE_A_BB;
    Bitboard shifted = shift<EAST>(b);
    REQUIRE(shifted == FILE_B_BB);

    // H file should not wrap to A file
    b = FILE_H_BB;
    shifted = shift<EAST>(b);
    REQUIRE(shifted == 0);
}

TEST_CASE("shift<WEST> moves bits left without wrap", "[bitboard][shift]") {
    Bitboard b = FILE_H_BB;
    Bitboard shifted = shift<WEST>(b);
    REQUIRE(shifted == FILE_G_BB);

    // A file should not wrap to H file
    b = FILE_A_BB;
    shifted = shift<WEST>(b);
    REQUIRE(shifted == 0);
}

TEST_CASE("diagonal shifts work correctly", "[bitboard][shift]") {
    Bitboard b = square_bb(SQ_E4);

    SECTION("NORTH_EAST") {
        Bitboard ne = shift<NORTH_EAST>(b);
        REQUIRE(ne == square_bb(SQ_F5));
    }

    SECTION("NORTH_WEST") {
        Bitboard nw = shift<NORTH_WEST>(b);
        REQUIRE(nw == square_bb(SQ_D5));
    }

    SECTION("SOUTH_EAST") {
        Bitboard se = shift<SOUTH_EAST>(b);
        REQUIRE(se == square_bb(SQ_F3));
    }

    SECTION("SOUTH_WEST") {
        Bitboard sw = shift<SOUTH_WEST>(b);
        REQUIRE(sw == square_bb(SQ_D3));
    }
}

TEST_CASE("diagonal shifts respect edges", "[bitboard][shift]") {
    // Corner A1 - only NE should work
    Bitboard a1 = square_bb(SQ_A1);
    REQUIRE(shift<NORTH_EAST>(a1) == square_bb(SQ_B2));
    REQUIRE(shift<NORTH_WEST>(a1) == 0);
    REQUIRE(shift<SOUTH_EAST>(a1) == 0);
    REQUIRE(shift<SOUTH_WEST>(a1) == 0);

    // Corner H8 - only SW should work
    Bitboard h8 = square_bb(SQ_H8);
    REQUIRE(shift<SOUTH_WEST>(h8) == square_bb(SQ_G7));
    REQUIRE(shift<NORTH_EAST>(h8) == 0);
    REQUIRE(shift<NORTH_WEST>(h8) == 0);
    REQUIRE(shift<SOUTH_EAST>(h8) == 0);
}

// ============================================================================
// Attack Table Tests
// ============================================================================

TEST_CASE("pawn_attacks_bb returns correct attacks", "[bitboard][attacks]") {
    SECTION("White pawn on E4") {
        Bitboard attacks = pawn_attacks_bb(WHITE, SQ_E4);
        REQUIRE((attacks & SQ_D5) != 0);
        REQUIRE((attacks & SQ_F5) != 0);
        REQUIRE(popcount(attacks) == 2);
    }

    SECTION("Black pawn on E4") {
        Bitboard attacks = pawn_attacks_bb(BLACK, SQ_E4);
        REQUIRE((attacks & SQ_D3) != 0);
        REQUIRE((attacks & SQ_F3) != 0);
        REQUIRE(popcount(attacks) == 2);
    }

    SECTION("White pawn on A file") {
        Bitboard attacks = pawn_attacks_bb(WHITE, SQ_A4);
        REQUIRE((attacks & SQ_B5) != 0);
        REQUIRE(popcount(attacks) == 1);
    }

    SECTION("Black pawn on H file") {
        Bitboard attacks = pawn_attacks_bb(BLACK, SQ_H4);
        REQUIRE((attacks & SQ_G3) != 0);
        REQUIRE(popcount(attacks) == 1);
    }
}

TEST_CASE("knight_attacks_bb returns correct attacks", "[bitboard][attacks]") {
    SECTION("Knight on E4 - center") {
        Bitboard attacks = knight_attacks_bb(SQ_E4);
        REQUIRE(popcount(attacks) == 8);
        REQUIRE((attacks & SQ_D6) != 0);
        REQUIRE((attacks & SQ_F6) != 0);
        REQUIRE((attacks & SQ_G5) != 0);
        REQUIRE((attacks & SQ_G3) != 0);
        REQUIRE((attacks & SQ_F2) != 0);
        REQUIRE((attacks & SQ_D2) != 0);
        REQUIRE((attacks & SQ_C3) != 0);
        REQUIRE((attacks & SQ_C5) != 0);
    }

    SECTION("Knight on A1 - corner") {
        Bitboard attacks = knight_attacks_bb(SQ_A1);
        REQUIRE(popcount(attacks) == 2);
        REQUIRE((attacks & SQ_B3) != 0);
        REQUIRE((attacks & SQ_C2) != 0);
    }

    SECTION("Knight on H8 - corner") {
        Bitboard attacks = knight_attacks_bb(SQ_H8);
        REQUIRE(popcount(attacks) == 2);
        REQUIRE((attacks & SQ_F7) != 0);
        REQUIRE((attacks & SQ_G6) != 0);
    }
}

TEST_CASE("king_attacks_bb returns correct attacks", "[bitboard][attacks]") {
    SECTION("King on E4 - center") {
        Bitboard attacks = king_attacks_bb(SQ_E4);
        REQUIRE(popcount(attacks) == 8);
        REQUIRE((attacks & SQ_D3) != 0);
        REQUIRE((attacks & SQ_E3) != 0);
        REQUIRE((attacks & SQ_F3) != 0);
        REQUIRE((attacks & SQ_D4) != 0);
        REQUIRE((attacks & SQ_F4) != 0);
        REQUIRE((attacks & SQ_D5) != 0);
        REQUIRE((attacks & SQ_E5) != 0);
        REQUIRE((attacks & SQ_F5) != 0);
    }

    SECTION("King on A1 - corner") {
        Bitboard attacks = king_attacks_bb(SQ_A1);
        REQUIRE(popcount(attacks) == 3);
        REQUIRE((attacks & SQ_A2) != 0);
        REQUIRE((attacks & SQ_B1) != 0);
        REQUIRE((attacks & SQ_B2) != 0);
    }
}

TEST_CASE("pawn_attacks_bb from bitboard", "[bitboard][attacks]") {
    SECTION("White pawns attack") {
        Bitboard pawns = square_bb(SQ_E4) | square_bb(SQ_D4);
        Bitboard attacks = pawn_attacks_bb(WHITE, pawns);
        REQUIRE((attacks & SQ_C5) != 0);
        REQUIRE((attacks & SQ_D5) != 0);
        REQUIRE((attacks & SQ_E5) != 0);
        REQUIRE((attacks & SQ_F5) != 0);
    }
}

// ============================================================================
// Line and Between Tests
// ============================================================================

TEST_CASE("between_bb returns squares between two squares", "[bitboard][line]") {
    SECTION("Horizontal") {
        Bitboard between = between_bb(SQ_A1, SQ_H1);
        REQUIRE((between & SQ_B1) != 0);
        REQUIRE((between & SQ_G1) != 0);
        REQUIRE((between & SQ_A1) == 0); // Endpoints not included
        REQUIRE((between & SQ_H1) == 0);
        REQUIRE(popcount(between) == 6);
    }

    SECTION("Vertical") {
        Bitboard between = between_bb(SQ_E1, SQ_E8);
        REQUIRE((between & SQ_E2) != 0);
        REQUIRE((between & SQ_E7) != 0);
        REQUIRE(popcount(between) == 6);
    }

    SECTION("Diagonal") {
        Bitboard between = between_bb(SQ_A1, SQ_H8);
        REQUIRE((between & SQ_B2) != 0);
        REQUIRE((between & SQ_G7) != 0);
        REQUIRE(popcount(between) == 6);
    }

    SECTION("Adjacent squares") {
        Bitboard between = between_bb(SQ_E4, SQ_E5);
        REQUIRE(between == 0);
    }

    SECTION("Same square") {
        Bitboard between = between_bb(SQ_E4, SQ_E4);
        REQUIRE(between == 0);
    }

    SECTION("Non-aligned squares") {
        Bitboard between = between_bb(SQ_E4, SQ_G7);
        REQUIRE(between == 0);
    }
}

TEST_CASE("line_bb returns line through two squares", "[bitboard][line]") {
    SECTION("Horizontal line") {
        Bitboard line = line_bb(SQ_C1, SQ_F1);
        REQUIRE((line & SQ_A1) != 0);
        REQUIRE((line & SQ_H1) != 0);
    }

    SECTION("Vertical line") {
        Bitboard line = line_bb(SQ_E3, SQ_E6);
        REQUIRE((line & SQ_E1) != 0);
        REQUIRE((line & SQ_E8) != 0);
    }

    SECTION("Diagonal line") {
        Bitboard line = line_bb(SQ_C3, SQ_F6);
        REQUIRE((line & SQ_A1) != 0);
        REQUIRE((line & SQ_H8) != 0);
    }
}

TEST_CASE("aligned detects three aligned squares", "[bitboard][line]") {
    SECTION("Aligned horizontally") {
        REQUIRE(aligned(SQ_A1, SQ_D1, SQ_H1));
    }

    SECTION("Aligned vertically") {
        REQUIRE(aligned(SQ_E1, SQ_E4, SQ_E8));
    }

    SECTION("Aligned diagonally") {
        REQUIRE(aligned(SQ_A1, SQ_D4, SQ_H8));
    }

    SECTION("Not aligned") {
        REQUIRE_FALSE(aligned(SQ_A1, SQ_B2, SQ_C4));
    }
}

// ============================================================================
// Constants Tests
// ============================================================================

TEST_CASE("Bitboard constants are correct", "[bitboard][constants]") {
    REQUIRE(EMPTY_BB == 0);
    REQUIRE(FULL_BB == ~0ULL);
    REQUIRE(popcount(FILE_A_BB) == 8);
    REQUIRE(popcount(RANK_1_BB) == 8);
    REQUIRE((FILE_A_BB & RANK_1_BB) == square_bb(SQ_A1));
    REQUIRE((FILE_H_BB & RANK_8_BB) == square_bb(SQ_H8));
}

TEST_CASE("NOT_FILE masks are correct", "[bitboard][constants]") {
    REQUIRE((NOT_FILE_A_BB & FILE_A_BB) == 0);
    REQUIRE((NOT_FILE_H_BB & FILE_H_BB) == 0);
    REQUIRE((NOT_FILE_AB_BB & (FILE_A_BB | FILE_B_BB)) == 0);
    REQUIRE((NOT_FILE_GH_BB & (FILE_G_BB | FILE_H_BB)) == 0);
}
