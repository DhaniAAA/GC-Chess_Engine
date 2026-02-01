// ============================================================================
// test_move.cpp - Unit Tests for Move Representation (move.hpp)
// ============================================================================

// Test framework macros defined in test_main.cpp
#include "../include/move.hpp"

// ============================================================================
// Move Construction Tests
// ============================================================================

TEST_CASE("Move default constructor creates MOVE_NONE", "[move][construction]") {
    Move m;
    REQUIRE(m == MOVE_NONE);
    REQUIRE(m.is_none());
    REQUIRE_FALSE(m.is_ok());
}

TEST_CASE("Move::make creates normal moves", "[move][construction]") {
    Move m = Move::make(SQ_E2, SQ_E4);
    REQUIRE(m.from() == SQ_E2);
    REQUIRE(m.to() == SQ_E4);
    REQUIRE(m.type() == NORMAL);
    REQUIRE(m.is_normal());
    REQUIRE(m.is_ok());
}

TEST_CASE("Move::make_promotion creates promotion moves", "[move][construction]") {
    SECTION("Promotion to Queen") {
        Move m = Move::make_promotion(SQ_E7, SQ_E8, QUEEN);
        REQUIRE(m.from() == SQ_E7);
        REQUIRE(m.to() == SQ_E8);
        REQUIRE(m.is_promotion());
        REQUIRE(m.promotion_type() == QUEEN);
    }

    SECTION("Promotion to Knight") {
        Move m = Move::make_promotion(SQ_A7, SQ_A8, KNIGHT);
        REQUIRE(m.is_promotion());
        REQUIRE(m.promotion_type() == KNIGHT);
    }

    SECTION("Promotion to Rook") {
        Move m = Move::make_promotion(SQ_H7, SQ_H8, ROOK);
        REQUIRE(m.is_promotion());
        REQUIRE(m.promotion_type() == ROOK);
    }

    SECTION("Promotion to Bishop") {
        Move m = Move::make_promotion(SQ_D7, SQ_D8, BISHOP);
        REQUIRE(m.is_promotion());
        REQUIRE(m.promotion_type() == BISHOP);
    }
}

TEST_CASE("Move::make_enpassant creates en passant moves", "[move][construction]") {
    Move m = Move::make_enpassant(SQ_E5, SQ_D6);
    REQUIRE(m.from() == SQ_E5);
    REQUIRE(m.to() == SQ_D6);
    REQUIRE(m.is_enpassant());
    REQUIRE(m.type() == EN_PASSANT);
}

TEST_CASE("Move::make_castling creates castling moves", "[move][construction]") {
    SECTION("White kingside") {
        Move m = Move::make_castling(SQ_E1, SQ_G1);
        REQUIRE(m.from() == SQ_E1);
        REQUIRE(m.to() == SQ_G1);
        REQUIRE(m.is_castling());
        REQUIRE(m.type() == CASTLING);
    }

    SECTION("White queenside") {
        Move m = Move::make_castling(SQ_E1, SQ_C1);
        REQUIRE(m.from() == SQ_E1);
        REQUIRE(m.to() == SQ_C1);
        REQUIRE(m.is_castling());
    }

    SECTION("Black kingside") {
        Move m = Move::make_castling(SQ_E8, SQ_G8);
        REQUIRE(m.from() == SQ_E8);
        REQUIRE(m.to() == SQ_G8);
        REQUIRE(m.is_castling());
    }

    SECTION("Black queenside") {
        Move m = Move::make_castling(SQ_E8, SQ_C8);
        REQUIRE(m.from() == SQ_E8);
        REQUIRE(m.to() == SQ_C8);
        REQUIRE(m.is_castling());
    }
}

// ============================================================================
// Move Accessor Tests
// ============================================================================

TEST_CASE("Move::from extracts source square", "[move][accessor]") {
    Move m1 = Move::make(SQ_A1, SQ_H8);
    REQUIRE(m1.from() == SQ_A1);

    Move m2 = Move::make(SQ_H8, SQ_A1);
    REQUIRE(m2.from() == SQ_H8);

    Move m3 = Move::make(SQ_E4, SQ_D5);
    REQUIRE(m3.from() == SQ_E4);
}

TEST_CASE("Move::to extracts target square", "[move][accessor]") {
    Move m1 = Move::make(SQ_A1, SQ_H8);
    REQUIRE(m1.to() == SQ_H8);

    Move m2 = Move::make(SQ_H8, SQ_A1);
    REQUIRE(m2.to() == SQ_A1);
}

TEST_CASE("Move::type returns correct move type", "[move][accessor]") {
    REQUIRE(Move::make(SQ_E2, SQ_E4).type() == NORMAL);
    REQUIRE(Move::make_promotion(SQ_E7, SQ_E8, QUEEN).type() == PROMOTION);
    REQUIRE(Move::make_enpassant(SQ_E5, SQ_D6).type() == EN_PASSANT);
    REQUIRE(Move::make_castling(SQ_E1, SQ_G1).type() == CASTLING);
}

// ============================================================================
// Move Type Check Tests
// ============================================================================

TEST_CASE("Move type check methods work correctly", "[move][type]") {
    Move normal = Move::make(SQ_E2, SQ_E4);
    Move promo = Move::make_promotion(SQ_E7, SQ_E8, QUEEN);
    Move ep = Move::make_enpassant(SQ_E5, SQ_D6);
    Move castle = Move::make_castling(SQ_E1, SQ_G1);

    SECTION("is_normal") {
        REQUIRE(normal.is_normal());
        REQUIRE_FALSE(promo.is_normal());
        REQUIRE_FALSE(ep.is_normal());
        REQUIRE_FALSE(castle.is_normal());
    }

    SECTION("is_promotion") {
        REQUIRE_FALSE(normal.is_promotion());
        REQUIRE(promo.is_promotion());
        REQUIRE_FALSE(ep.is_promotion());
        REQUIRE_FALSE(castle.is_promotion());
    }

    SECTION("is_enpassant") {
        REQUIRE_FALSE(normal.is_enpassant());
        REQUIRE_FALSE(promo.is_enpassant());
        REQUIRE(ep.is_enpassant());
        REQUIRE_FALSE(castle.is_enpassant());
    }

    SECTION("is_castling") {
        REQUIRE_FALSE(normal.is_castling());
        REQUIRE_FALSE(promo.is_castling());
        REQUIRE_FALSE(ep.is_castling());
        REQUIRE(castle.is_castling());
    }
}

// ============================================================================
// Move Validity Tests
// ============================================================================

TEST_CASE("Move validity checks", "[move][validity]") {
    SECTION("is_ok for valid moves") {
        REQUIRE(Move::make(SQ_E2, SQ_E4).is_ok());
        REQUIRE(Move::make_promotion(SQ_E7, SQ_E8, QUEEN).is_ok());
        REQUIRE(Move::make_enpassant(SQ_E5, SQ_D6).is_ok());
        REQUIRE(Move::make_castling(SQ_E1, SQ_G1).is_ok());
    }

    SECTION("is_none for null move") {
        REQUIRE(MOVE_NONE.is_none());
        REQUIRE_FALSE(Move::make(SQ_E2, SQ_E4).is_none());
    }

    SECTION("MOVE_NULL is special marker") {
        REQUIRE(MOVE_NULL != MOVE_NONE);
    }
}

// ============================================================================
// Move Comparison Tests
// ============================================================================

TEST_CASE("Move comparison operators", "[move][comparison]") {
    Move m1 = Move::make(SQ_E2, SQ_E4);
    Move m2 = Move::make(SQ_E2, SQ_E4);
    Move m3 = Move::make(SQ_D2, SQ_D4);

    REQUIRE(m1 == m2);
    REQUIRE_FALSE(m1 != m2);
    REQUIRE(m1 != m3);
    REQUIRE_FALSE(m1 == m3);
}

TEST_CASE("Move raw data access", "[move][raw]") {
    Move m = Move::make(SQ_E2, SQ_E4);
    U16 raw = m.raw();
    Move m2(raw);
    REQUIRE(m == m2);
}

// ============================================================================
// Move String Conversion Tests
// ============================================================================

TEST_CASE("move_to_string converts correctly", "[move][string]") {
    SECTION("Normal moves") {
        REQUIRE(move_to_string(Move::make(SQ_E2, SQ_E4)) == "e2e4");
        REQUIRE(move_to_string(Move::make(SQ_A1, SQ_H8)) == "a1h8");
        REQUIRE(move_to_string(Move::make(SQ_G1, SQ_F3)) == "g1f3");
    }

    SECTION("Promotion moves") {
        REQUIRE(move_to_string(Move::make_promotion(SQ_E7, SQ_E8, QUEEN)) == "e7e8q");
        REQUIRE(move_to_string(Move::make_promotion(SQ_A7, SQ_A8, KNIGHT)) == "a7a8n");
        REQUIRE(move_to_string(Move::make_promotion(SQ_H7, SQ_H8, ROOK)) == "h7h8r");
        REQUIRE(move_to_string(Move::make_promotion(SQ_B7, SQ_B8, BISHOP)) == "b7b8b");
    }

    SECTION("Capture promotions") {
        REQUIRE(move_to_string(Move::make_promotion(SQ_E7, SQ_D8, QUEEN)) == "e7d8q");
        REQUIRE(move_to_string(Move::make_promotion(SQ_E7, SQ_F8, KNIGHT)) == "e7f8n");
    }

    SECTION("Black promotions") {
        REQUIRE(move_to_string(Move::make_promotion(SQ_E2, SQ_E1, QUEEN)) == "e2e1q");
        REQUIRE(move_to_string(Move::make_promotion(SQ_A2, SQ_B1, KNIGHT)) == "a2b1n");
    }

    SECTION("Castling moves") {
        REQUIRE(move_to_string(Move::make_castling(SQ_E1, SQ_G1)) == "e1g1");
        REQUIRE(move_to_string(Move::make_castling(SQ_E1, SQ_C1)) == "e1c1");
        REQUIRE(move_to_string(Move::make_castling(SQ_E8, SQ_G8)) == "e8g8");
        REQUIRE(move_to_string(Move::make_castling(SQ_E8, SQ_C8)) == "e8c8");
    }

    SECTION("En passant moves") {
        REQUIRE(move_to_string(Move::make_enpassant(SQ_E5, SQ_D6)) == "e5d6");
        REQUIRE(move_to_string(Move::make_enpassant(SQ_D5, SQ_E6)) == "d5e6");
    }

    SECTION("MOVE_NONE") {
        REQUIRE(move_to_string(MOVE_NONE) == "0000");
    }
}

TEST_CASE("string_to_move parses correctly", "[move][string]") {
    SECTION("Normal moves") {
        Move m = string_to_move("e2e4");
        REQUIRE(m.from() == SQ_E2);
        REQUIRE(m.to() == SQ_E4);
    }

    SECTION("Promotion moves") {
        Move mq = string_to_move("e7e8q");
        REQUIRE(mq.from() == SQ_E7);
        REQUIRE(mq.to() == SQ_E8);
        REQUIRE(mq.is_promotion());
        REQUIRE(mq.promotion_type() == QUEEN);

        Move mn = string_to_move("a7a8n");
        REQUIRE(mn.is_promotion());
        REQUIRE(mn.promotion_type() == KNIGHT);

        Move mr = string_to_move("h7h8r");
        REQUIRE(mr.is_promotion());
        REQUIRE(mr.promotion_type() == ROOK);

        Move mb = string_to_move("b7b8b");
        REQUIRE(mb.is_promotion());
        REQUIRE(mb.promotion_type() == BISHOP);
    }

    SECTION("Case insensitive promotion") {
        Move mQ = string_to_move("e7e8Q");
        REQUIRE(mQ.is_promotion());
        REQUIRE(mQ.promotion_type() == QUEEN);
    }

    SECTION("Invalid moves") {
        REQUIRE(string_to_move("") == MOVE_NONE);
        REQUIRE(string_to_move("e2") == MOVE_NONE);
        REQUIRE(string_to_move("xyz") == MOVE_NONE);
        REQUIRE(string_to_move("i1i2") == MOVE_NONE);
    }
}

// ============================================================================
// ScoredMove Tests
// ============================================================================

TEST_CASE("ScoredMove construction", "[move][scored]") {
    SECTION("Default constructor") {
        ScoredMove sm;
        REQUIRE(sm.move == MOVE_NONE);
        REQUIRE(sm.score == 0);
    }

    SECTION("Parameterized constructor") {
        Move m = Move::make(SQ_E2, SQ_E4);
        ScoredMove sm(m, 100);
        REQUIRE(sm.move == m);
        REQUIRE(sm.score == 100);
    }
}

TEST_CASE("ScoredMove comparison", "[move][scored]") {
    ScoredMove a(Move::make(SQ_E2, SQ_E4), 100);
    ScoredMove b(Move::make(SQ_D2, SQ_D4), 50);
    ScoredMove c(Move::make(SQ_G1, SQ_F3), 100);

    // Higher score = better (operator< returns true for better moves)
    REQUIRE(a < b);  // a has higher score, so a is "less" in sort order (comes first)
    REQUIRE_FALSE(b < a);
}

// ============================================================================
// MoveList Tests
// ============================================================================

TEST_CASE("MoveList basic operations", "[move][movelist]") {
    MoveList ml;

    SECTION("Empty list") {
        REQUIRE(ml.size() == 0);
        REQUIRE(ml.empty());
    }

    SECTION("Add moves") {
        ml.clear();
        ml.add(Move::make(SQ_E2, SQ_E4));
        REQUIRE(ml.size() == 1);
        REQUIRE_FALSE(ml.empty());

        ml.add(Move::make(SQ_D2, SQ_D4));
        REQUIRE(ml.size() == 2);
    }

    SECTION("Add with score") {
        ml.clear();
        ml.add(Move::make(SQ_E2, SQ_E4), 100);
        REQUIRE(ml[0].move == Move::make(SQ_E2, SQ_E4));
        REQUIRE(ml[0].score == 100);
    }

    SECTION("Clear list") {
        ml.clear();
        ml.add(Move::make(SQ_E2, SQ_E4));
        ml.add(Move::make(SQ_D2, SQ_D4));
        ml.clear();
        REQUIRE(ml.size() == 0);
        REQUIRE(ml.empty());
    }
}

TEST_CASE("MoveList pick_best", "[move][movelist]") {
    MoveList ml;
    ml.add(Move::make(SQ_A2, SQ_A3), 10);
    ml.add(Move::make(SQ_E2, SQ_E4), 100);
    ml.add(Move::make(SQ_D2, SQ_D4), 50);

    Move best = ml.pick_best(0);
    REQUIRE(best == Move::make(SQ_E2, SQ_E4));
    REQUIRE(ml[0].move == Move::make(SQ_E2, SQ_E4)); // Swapped to front

    Move second = ml.pick_best(1);
    REQUIRE(second == Move::make(SQ_D2, SQ_D4));
}

TEST_CASE("MoveList iteration", "[move][movelist]") {
    MoveList ml;
    ml.add(Move::make(SQ_E2, SQ_E4));
    ml.add(Move::make(SQ_D2, SQ_D4));
    ml.add(Move::make(SQ_G1, SQ_F3));

    int count = 0;
    for (auto& sm : ml) {
        count++;
        REQUIRE(sm.move.is_ok());
    }
    REQUIRE(count == 3);
}
