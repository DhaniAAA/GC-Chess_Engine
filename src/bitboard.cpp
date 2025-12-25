#include "bitboard.hpp"
#include <iostream>
#include <sstream>

// ============================================================================
// Global Lookup Tables
// ============================================================================

Bitboard FileBB[FILE_NB];
Bitboard RankBB[RANK_NB];
Bitboard SquareBB[SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];

// ============================================================================
// Private Helper Functions
// ============================================================================

namespace {

// Generate sliding attacks (used for initialization only)
Bitboard sliding_attack(PieceType pt, Square sq, Bitboard occupied) {
    Bitboard attacks = EMPTY_BB;

    Direction rook_directions[4] = {NORTH, SOUTH, EAST, WEST};
    Direction bishop_directions[4] = {NORTH_EAST, NORTH_WEST, SOUTH_EAST, SOUTH_WEST};

    Direction* directions = (pt == ROOK) ? rook_directions : bishop_directions;

    for (int i = 0; i < 4; ++i) {
        Direction d = directions[i];
        Square s = sq;

        while (true) {
            // Check boundaries before moving
            File f = file_of(s);
            Rank r = rank_of(s);

            // Prevent wrapping
            bool can_move = true;
            if (d == EAST || d == NORTH_EAST || d == SOUTH_EAST) {
                can_move = (f < FILE_H);
            }
            if (d == WEST || d == NORTH_WEST || d == SOUTH_WEST) {
                can_move = can_move && (f > FILE_A);
            }
            if (d == NORTH || d == NORTH_EAST || d == NORTH_WEST) {
                can_move = can_move && (r < RANK_8);
            }
            if (d == SOUTH || d == SOUTH_EAST || d == SOUTH_WEST) {
                can_move = can_move && (r > RANK_1);
            }

            if (!can_move) break;

            s = s + d;
            attacks |= square_bb(s);

            if (occupied & s) break; // Blocked by piece
        }
    }

    return attacks;
}

// Initialize pawn attack tables
void init_pawn_attacks() {
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Bitboard sq_bb = square_bb(s);

        // White pawn attacks
        PawnAttacks[WHITE][s] = shift<NORTH_WEST>(sq_bb) | shift<NORTH_EAST>(sq_bb);

        // Black pawn attacks
        PawnAttacks[BLACK][s] = shift<SOUTH_WEST>(sq_bb) | shift<SOUTH_EAST>(sq_bb);
    }
}

// Initialize knight attack tables
void init_knight_attacks() {
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Bitboard attacks = EMPTY_BB;
        Bitboard sq_bb = square_bb(s);

        // All 8 knight move patterns
        attacks |= (sq_bb & NOT_FILE_A_BB) << 15;           // NNW
        attacks |= (sq_bb & NOT_FILE_H_BB) << 17;           // NNE
        attacks |= (sq_bb & NOT_FILE_AB_BB) << 6;           // NWW
        attacks |= (sq_bb & NOT_FILE_GH_BB) << 10;          // NEE
        attacks |= (sq_bb & NOT_FILE_A_BB) >> 17;           // SSW
        attacks |= (sq_bb & NOT_FILE_H_BB) >> 15;           // SSE
        attacks |= (sq_bb & NOT_FILE_AB_BB) >> 10;          // SWW
        attacks |= (sq_bb & NOT_FILE_GH_BB) >> 6;           // SEE

        KnightAttacks[s] = attacks;
    }
}

// Initialize king attack tables
void init_king_attacks() {
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Bitboard sq_bb = square_bb(s);

        KingAttacks[s] = shift<NORTH>(sq_bb) | shift<SOUTH>(sq_bb) |
                         shift<EAST>(sq_bb)  | shift<WEST>(sq_bb)  |
                         shift<NORTH_EAST>(sq_bb) | shift<NORTH_WEST>(sq_bb) |
                         shift<SOUTH_EAST>(sq_bb) | shift<SOUTH_WEST>(sq_bb);
    }
}

// Initialize between and line bitboards
void init_between_line() {
    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
        for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2) {
            BetweenBB[s1][s2] = EMPTY_BB;
            LineBB[s1][s2] = EMPTY_BB;

            if (s1 == s2) continue;

            // Check if squares are on same rank, file, or diagonal
            File f1 = file_of(s1), f2 = file_of(s2);
            Rank r1 = rank_of(s1), r2 = rank_of(s2);

            int df = f2 - f1;
            int dr = r2 - r1;

            // Check if aligned (same rank, file, or diagonal)
            bool aligned = (f1 == f2) || (r1 == r2) ||
                          (df == dr) || (df == -dr);

            if (!aligned) continue;

            // Determine direction
            int step_f = (df > 0) ? 1 : (df < 0) ? -1 : 0;
            int step_r = (dr > 0) ? 1 : (dr < 0) ? -1 : 0;

            // Build between bitboard (squares strictly between s1 and s2)
            Bitboard between = EMPTY_BB;
            File f = File(f1 + step_f);
            Rank r = Rank(r1 + step_r);

            while (f != f2 || r != r2) {
                between |= square_bb(make_square(f, r));
                f = File(f + step_f);
                r = Rank(r + step_r);
            }

            BetweenBB[s1][s2] = between;

            // Build line bitboard (entire line through s1 and s2)
            // IMPORTANT: Use only the appropriate slider type!
            // If same file or same rank: use ROOK attacks only
            // If same diagonal: use BISHOP attacks only

            if (f1 == f2 || r1 == r2) {
                // Same file or rank - use ROOK attacks
                LineBB[s1][s2] = (sliding_attack(ROOK, s1, EMPTY_BB) &
                                 sliding_attack(ROOK, s2, EMPTY_BB)) | s1 | s2;
            } else if (df == dr || df == -dr) {
                // Same diagonal - use BISHOP attacks
                LineBB[s1][s2] = (sliding_attack(BISHOP, s1, EMPTY_BB) &
                                 sliding_attack(BISHOP, s2, EMPTY_BB)) | s1 | s2;
            }
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public Functions
// ============================================================================

namespace Bitboards {

void init() {
    // Initialize file and rank bitboards
    for (File f = FILE_A; f <= FILE_H; ++f) {
        FileBB[f] = FILE_A_BB << f;
    }

    for (Rank r = RANK_1; r <= RANK_8; ++r) {
        RankBB[r] = RANK_1_BB << (8 * r);
    }

    // Initialize square bitboards
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        SquareBB[s] = 1ULL << s;
    }

    // Initialize attack tables
    init_pawn_attacks();
    init_knight_attacks();
    init_king_attacks();
    init_between_line();
}

std::string pretty(Bitboard b) {
    std::ostringstream ss;
    ss << "+---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_8; r >= RANK_1; --r) {
        for (File f = FILE_A; f <= FILE_H; ++f) {
            Square s = make_square(f, r);
            ss << "| " << ((b & s) ? "X " : ". ");
        }
        ss << "| " << (r + 1) << "\n";
        ss << "+---+---+---+---+---+---+---+---+\n";
    }
    ss << "  a   b   c   d   e   f   g   h\n";

    return ss.str();
}

} // namespace Bitboards
