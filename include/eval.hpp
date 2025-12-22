#ifndef EVAL_HPP
#define EVAL_HPP

#include "board.hpp"
#include "magic.hpp"
#include <algorithm>
#include "tuning.hpp"

namespace Eval {

// ============================================================================
// Evaluation Score Structure (MG and EG packed)
// ============================================================================

// EvalScore structure and S helper moved to types.hpp

// ============================================================================
// Material Values (MG, EG)
// ============================================================================

    using Tuning::PawnValue;
    using Tuning::KnightValue;
    using Tuning::BishopValue;
    using Tuning::RookValue;
    using Tuning::QueenValue;

// Game phase values for tapered evaluation
constexpr int PhaseValue[PIECE_TYPE_NB] = { 0, 0, 1, 1, 2, 4, 0 };
constexpr int TotalPhase = 24;  // 4*1 (knights) + 4*1 (bishops) + 4*2 (rooks) + 2*4 (queens)

// ============================================================================
// Piece-Square Tables (Middlegame and Endgame)
// ============================================================================

// Pawn PST (from white's perspective)
constexpr EvalScore PawnPST[SQUARE_NB] = {
    S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0),
    S( -1,  0), S(  4,  0), S( -8,  0), S(-24,  0), S(-24,  0), S( -8,  0), S(  4,  0), S( -1,  0),
    S( -2,  0), S(  2,  0), S(  0,  0), S( -4,  0), S( -4,  0), S(  0,  0), S(  2,  0), S( -2,  0),
    S(  0,  5), S(  0,  5), S(  4, 10), S( 18, 15), S( 18, 15), S(  4, 10), S(  0,  5), S(  0,  5),
    S(  5, 10), S( 10, 15), S( 15, 20), S( 25, 25), S( 25, 25), S( 15, 20), S( 10, 15), S(  5, 10),
    S( 10, 25), S( 20, 35), S( 30, 45), S( 40, 55), S( 40, 55), S( 30, 45), S( 20, 35), S( 10, 25),
    S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70),
    S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0)
};

// Knight PST
constexpr EvalScore KnightPST[SQUARE_NB] = {
    S(-50,-50), S(-40,-40), S(-30,-30), S(-30,-30), S(-30,-30), S(-30,-30), S(-40,-40), S(-50,-50),
    S(-40,-40), S(-20,-20), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(-20,-20), S(-40,-40),
    S(-30,-30), S(  5,  0), S( 15, 10), S( 20, 15), S( 20, 15), S( 15, 10), S(  5,  0), S(-30,-30),
    S(-30,-25), S(  5,  5), S( 20, 15), S( 25, 25), S( 25, 25), S( 20, 15), S(  5,  5), S(-30,-25),
    S(-30,-25), S( 10,  5), S( 20, 15), S( 25, 25), S( 25, 25), S( 20, 15), S( 10,  5), S(-30,-25),
    S(-30,-30), S(  5,  0), S( 15, 10), S( 20, 15), S( 20, 15), S( 15, 10), S(  5,  0), S(-30,-30),
    S(-40,-40), S(-20,-20), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(-20,-20), S(-40,-40),
    S(-50,-50), S(-40,-40), S(-30,-30), S(-30,-30), S(-30,-30), S(-30,-30), S(-40,-40), S(-50,-50)
};

// Bishop PST
constexpr EvalScore BishopPST[SQUARE_NB] = {
    S(-20,-20), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-20,-20),
    S(-10,-10), S( 10,  5), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S( 10,  5), S(-10,-10),
    S(-10,-10), S( 10,  5), S( 10, 10), S( 10, 10), S( 10, 10), S( 10, 10), S( 10,  5), S(-10,-10),
    S(-10,-10), S(  0,  5), S( 10, 10), S( 15, 15), S( 15, 15), S( 10, 10), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  5,  5), S( 10, 10), S( 15, 15), S( 15, 15), S( 10, 10), S(  5,  5), S(-10,-10),
    S(-10,-10), S(  0,  5), S( 10, 10), S( 10, 10), S( 10, 10), S( 10, 10), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  5,  5), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(  5,  5), S(-10,-10),
    S(-20,-20), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-20,-20)
};

// Rook PST
constexpr EvalScore RookPST[SQUARE_NB] = {
    S(  0, -5), S(  0, -5), S(  5,  0), S( 10,  5), S( 10,  5), S(  5,  0), S(  0, -5), S(  0, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( 10,  5), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 10,  5),
    S(  0,  0), S(  0,  0), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(  0,  0), S(  0,  0)
};

// Queen PST
constexpr EvalScore QueenPST[SQUARE_NB] = {
    S(-20,-20), S(-10,-10), S(-10,-10), S( -5,-10), S( -5,-10), S(-10,-10), S(-10,-10), S(-20,-20),
    S(-10,-10), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(-10,-10),
    S(-10,-10), S(  0,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  0,  5), S(-10,-10),
    S( -5, -5), S(  0,  0), S(  5,  5), S(  5, 10), S(  5, 10), S(  5,  5), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  5,  5), S(  5, 10), S(  5, 10), S(  5,  5), S(  0,  0), S( -5, -5),
    S(-10,-10), S(  0,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(-10,-10),
    S(-20,-20), S(-10,-10), S(-10,-10), S( -5,-10), S( -5,-10), S(-10,-10), S(-10,-10), S(-20,-20)
};

// King PST Middlegame (prefer castled position)
constexpr int KingPSTMG[SQUARE_NB] = {
     20,  30,  10,   0,   0,  10,  30,  20,
     20,  20,   0, -10, -10,   0,  20,  20,
    -10, -20, -20, -30, -30, -20, -20, -10,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40
};

// King PST Endgame (prefer centralized king)
constexpr int KingPSTEG[SQUARE_NB] = {
    -50, -30, -20, -20, -20, -20, -30, -50,
    -30, -10,   0,  10,  10,   0, -10, -30,
    -20,   0,  15,  25,  25,  15,   0, -20,
    -20,  10,  25,  35,  35,  25,  10, -20,
    -20,  10,  25,  35,  35,  25,  10, -20,
    -20,   0,  15,  25,  25,  15,   0, -20,
    -30, -10,   0,  10,  10,   0, -10, -30,
    -50, -30, -20, -20, -20, -20, -30, -50
};

// ============================================================================
// Evaluation Bonuses and Penalties
// ============================================================================

// Pawn structure
constexpr EvalScore PassedPawnBonus[8] = {
    S(  0,   0), S(  5,  10), S( 10,  20), S( 20,  40),
    S( 40,  75), S( 70, 120), S(100, 180), S(  0,   0)
};
    using Tuning::IsolatedPawnPenalty;
    using Tuning::DoubledPawnPenalty;
    using Tuning::BackwardPawnPenalty;
    using Tuning::ConnectedPawnBonus;
    using Tuning::PhalanxBonus;

// Piece activity
    using Tuning::BishopPairBonus;
    using Tuning::RookOpenFileBonus;
    using Tuning::RookSemiOpenFileBonus;
    using Tuning::RookOnSeventhBonus;
    using Tuning::KnightOutpostBonus;

// Mobility bonus per square
constexpr EvalScore KnightMobility[9] = {
    S(-30, -40), S(-15, -20), S( -5, -10), S(  0,  0),
    S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
    S( 20,  20)
};
constexpr EvalScore BishopMobility[14] = {
    S(-25, -35), S(-15, -20), S( -5, -10), S(  0,  0),
    S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
    S( 20,  20), S( 22,  22), S( 24,  24), S( 25, 25),
    S( 26,  26), S( 27,  27)
};
constexpr EvalScore RookMobility[15] = {
    S(-20, -30), S(-12, -18), S( -5, -10), S(  0,  0),
    S(  5,   5), S(  8,  10), S( 10,  15), S( 12, 18),
    S( 14,  20), S( 16,  22), S( 17,  24), S( 18, 25),
    S( 19,  26), S( 20,  27), S( 20,  28)
};
constexpr EvalScore QueenMobility[28] = {
    S(-15, -25), S(-10, -15), S( -5, -10), S(  0,  0),
    S(  2,   3), S(  4,   5), S(  5,   7), S(  6,  8),
    S(  7,   9), S(  8,  10), S(  9,  11), S( 10, 12),
    S( 10,  13), S( 11,  13), S( 11,  14), S( 12, 14),
    S( 12,  15), S( 13,  15), S( 13,  16), S( 14, 16),
    S( 14,  17), S( 15,  17), S( 15,  18), S( 15, 18),
    S( 16,  18), S( 16,  19), S( 16,  19), S( 17, 20)
};

// King safety (attack unit weights)
constexpr int KnightAttackWeight = 2;
constexpr int BishopAttackWeight = 2;
constexpr int RookAttackWeight   = 3;
constexpr int QueenAttackWeight  = 5;

// King safety lookup table (based on attack units)
constexpr int KingSafetyTable[100] = {
    0,   0,   1,   2,   3,   5,   7,  10,  13,  16,
   20,  25,  30,  36,  42,  49,  56,  64,  72,  81,
   90, 100, 110, 121, 132, 144, 156, 169, 182, 196,
  210, 225, 240, 256, 272, 289, 306, 324, 342, 361,
  380, 400, 420, 441, 462, 484, 506, 529, 552, 576,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600
};

// Pawn shield bonus (MG only)
constexpr int PawnShieldBonus[4] = { 0, 10, 20, 30 };

// ============================================================================
// Helper Functions
// ============================================================================

inline Square flip_square(Square sq) {
    return Square(sq ^ 56);
}

// Get file mask for a file
inline Bitboard file_bb(File f) {
    return 0x0101010101010101ULL << f;
}

// Get adjacent files mask
inline Bitboard adjacent_files_bb(File f) {
    Bitboard result = 0;
    if (f > FILE_A) result |= file_bb(File(f - 1));
    if (f < FILE_H) result |= file_bb(File(f + 1));
    return result;
}

// Get rank mask for a rank
inline Bitboard rank_bb_eval(Rank r) {
    return 0xFFULL << (r * 8);
}

// Get squares in front of a pawn
inline Bitboard pawn_front_span(Color c, Square s) {
    Bitboard b = 0;
    File f = file_of(s);
    if (c == WHITE) {
        for (int r = rank_of(s) + 1; r <= RANK_8; ++r) {
            b |= square_bb(make_square(f, Rank(r)));
        }
    } else {
        for (int r = rank_of(s) - 1; r >= RANK_1; --r) {
            b |= square_bb(make_square(f, Rank(r)));
        }
    }
    return b;
}

// Get passed pawn mask (squares that must be free of enemy pawns)
inline Bitboard passed_pawn_mask(Color c, Square s) {
    Bitboard front = pawn_front_span(c, s);
    File f = file_of(s);
    if (f > FILE_A) front |= pawn_front_span(c, Square(s - 1));
    if (f < FILE_H) front |= pawn_front_span(c, Square(s + 1));
    return front;
}

// ============================================================================
// Evaluation Components
// ============================================================================

// Evaluate material and PST for one side
inline EvalScore eval_material_pst(const Board& board, Color c) {
    EvalScore score;
    Bitboard bb;

    // Pawns
    bb = board.pieces(c, PAWN);
    while (bb) {
        Square sq = pop_lsb(bb);
        Square psq = c == WHITE ? sq : flip_square(sq);
        score += PawnValue + PawnPST[psq];
    }

    // Knights
    bb = board.pieces(c, KNIGHT);
    while (bb) {
        Square sq = pop_lsb(bb);
        Square psq = c == WHITE ? sq : flip_square(sq);
        score += KnightValue + KnightPST[psq];
    }

    // Bishops
    bb = board.pieces(c, BISHOP);
    while (bb) {
        Square sq = pop_lsb(bb);
        Square psq = c == WHITE ? sq : flip_square(sq);
        score += BishopValue + BishopPST[psq];
    }

    // Rooks
    bb = board.pieces(c, ROOK);
    while (bb) {
        Square sq = pop_lsb(bb);
        Square psq = c == WHITE ? sq : flip_square(sq);
        score += RookValue + RookPST[psq];
    }

    // Queens
    bb = board.pieces(c, QUEEN);
    while (bb) {
        Square sq = pop_lsb(bb);
        Square psq = c == WHITE ? sq : flip_square(sq);
        score += QueenValue + QueenPST[psq];
    }

    // King
    Square kingSq = board.king_square(c);
    Square kingPsq = c == WHITE ? kingSq : flip_square(kingSq);
    score.mg += KingPSTMG[kingPsq];
    score.eg += KingPSTEG[kingPsq];

    return score;
}

// Evaluate pawn structure for one side
inline EvalScore eval_pawn_structure(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    Bitboard bb = ourPawns;
    while (bb) {
        Square sq = pop_lsb(bb);
        File f = file_of(sq);
        Rank r = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));

        // Passed pawn check
        if (!(passed_pawn_mask(c, sq) & theirPawns)) {
            score += PassedPawnBonus[r];
        }

        // Isolated pawn check
        if (!(adjacent_files_bb(f) & ourPawns)) {
            score += IsolatedPawnPenalty;
        }

        // Doubled pawn check
        if (popcount(file_bb(f) & ourPawns) > 1) {
            score += DoubledPawnPenalty;
        }

        // Connected pawn check
        Bitboard adjacentPawns = adjacent_files_bb(f) & ourPawns;
        if (adjacentPawns) {
            // Check for phalanx (pawns on same rank)
            if (adjacentPawns & rank_bb_eval(rank_of(sq))) {
                score += PhalanxBonus;
            } else {
                score += ConnectedPawnBonus;
            }
        }
    }

    return score;
}

// Evaluate piece activity (mobility, outposts, rooks on files)
inline EvalScore eval_pieces(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard occupied = board.pieces();
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Bitboard mobilityArea = ~(board.pieces(c) | pawn_attacks_bb(enemy, theirPawns));

    // Knights
    Bitboard bb = board.pieces(c, KNIGHT);
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = knight_attacks_bb(sq);
        int mobility = popcount(attacks & mobilityArea);
        score += KnightMobility[std::min(mobility, 8)];

        // Outpost check
        Bitboard pawnDefenders = pawn_attacks_bb(enemy, ourPawns);
        if ((square_bb(sq) & pawnDefenders)) {
            Rank relativeRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
            if (relativeRank >= RANK_4 && relativeRank <= RANK_6) {
                score += KnightOutpostBonus;
            }
        }
    }

    // Bishops
    bb = board.pieces(c, BISHOP);
    int bishopCount = 0;
    while (bb) {
        bishopCount++;
        Square sq = pop_lsb(bb);
        Bitboard attacks = bishop_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += BishopMobility[std::min(mobility, 13)];
    }
    // Bishop pair bonus
    if (bishopCount >= 2) {
        score += BishopPairBonus;
    }

    // Rooks
    bb = board.pieces(c, ROOK);
    while (bb) {
        Square sq = pop_lsb(bb);
        File f = file_of(sq);
        Bitboard attacks = rook_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += RookMobility[std::min(mobility, 14)];

        // Open/semi-open file bonus
        Bitboard filePawns = file_bb(f);
        if (!(filePawns & ourPawns)) {
            if (!(filePawns & theirPawns)) {
                score += RookOpenFileBonus;
            } else {
                score += RookSemiOpenFileBonus;
            }
        }

        // Rook on 7th rank
        Rank relativeRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        if (relativeRank == RANK_7) {
            score += RookOnSeventhBonus;
        }
    }

    // Queens
    bb = board.pieces(c, QUEEN);
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = queen_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += QueenMobility[std::min(mobility, 27)];
    }

    return score;
}

// Evaluate king safety
inline EvalScore eval_king_safety(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Square kingSq = board.king_square(c);
    Bitboard occupied = board.pieces();

    // King zone (3x3 area around king)
    Bitboard kingZone = king_attacks_bb(kingSq) | square_bb(kingSq);

    // Get all enemy attacks on king zone using pre-computed attack bitboards
    // This is faster than looping through each piece individually
    // Bitboard enemyPieces = board.pieces(enemy);

    int attackUnits = 0;
    int attackCount = 0;

    // Count attacks by piece type (optimized - avoid per-piece loop)
    Bitboard knightAttackers = board.pieces(enemy, KNIGHT);
    while (knightAttackers) {
        Square sq = pop_lsb(knightAttackers);
        if (knight_attacks_bb(sq) & kingZone) {
            attackUnits += KnightAttackWeight;
            attackCount++;
        }
    }

    // Sliders - only compute if there are relevant pieces
    Bitboard bishopQueens = board.pieces(enemy, BISHOP, QUEEN);
    Bitboard rookQueens = board.pieces(enemy, ROOK, QUEEN);

    if (bishopQueens) {
        Bitboard bishopAttacks = bishop_attacks_bb(kingSq, occupied);
        Bitboard attackers = bishopAttacks & bishopQueens;
        int count = popcount(attackers);
        if (count) {
            attackUnits += count * BishopAttackWeight;
            attackCount += count;
        }
    }

    if (rookQueens) {
        Bitboard rookAttacks = rook_attacks_bb(kingSq, occupied);
        Bitboard attackers = rookAttacks & rookQueens;
        int count = popcount(attackers);
        if (count) {
            attackUnits += count * RookAttackWeight;
            attackCount += count;
        }
    }

    // Only apply king safety penalty if there are enough attackers
    if (attackCount >= 2) {
        int safetyPenalty = KingSafetyTable[std::min(attackUnits, 99)];
        score.mg -= safetyPenalty;
    }

    // Pawn shield evaluation (for castled king) - simplified
    Rank kingRank = c == WHITE ? rank_of(kingSq) : Rank(RANK_8 - rank_of(kingSq));
    if (kingRank <= RANK_2) {
        File kingFile = file_of(kingSq);
        Bitboard ourPawns = board.pieces(c, PAWN);
        Bitboard shieldZone = c == WHITE ?
            (rank_bb_eval(RANK_2) | rank_bb_eval(RANK_3)) :
            (rank_bb_eval(RANK_6) | rank_bb_eval(RANK_7));

        // Count pawns in front of king (optimized)
        Bitboard shieldMask = 0;
        if (kingFile > FILE_A) shieldMask |= file_bb(File(kingFile - 1));
        shieldMask |= file_bb(kingFile);
        if (kingFile < FILE_H) shieldMask |= file_bb(File(kingFile + 1));

        int shieldCount = popcount(shieldMask & shieldZone & ourPawns);
        score.mg += PawnShieldBonus[std::min(shieldCount, 3)];
    }

    return score;
}

// ============================================================================
// Main Evaluation Function
// ============================================================================

inline int evaluate(const Board& board) {
    EvalScore score;

    // Calculate game phase
    int phase = TotalPhase;
    phase -= popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase -= popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase -= popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase -= popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::max(0, phase);  // Ensure non-negative

    // Material and PST
    score += eval_material_pst(board, WHITE);
    score -= eval_material_pst(board, BLACK);

    // Pawn structure
    score += eval_pawn_structure(board, WHITE);
    score -= eval_pawn_structure(board, BLACK);

    // Piece activity
    score += eval_pieces(board, WHITE);
    score -= eval_pieces(board, BLACK);

    // King safety
    score += eval_king_safety(board, WHITE);
    score -= eval_king_safety(board, BLACK);

    // Tapered evaluation
    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    // Return from side to move's perspective
    return board.side_to_move() == WHITE ? finalScore : -finalScore;
}

} // namespace Eval

#endif // EVAL_HPP
