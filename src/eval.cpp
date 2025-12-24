// ============================================================================
// eval.cpp - Evaluation Function Implementation
// ============================================================================
// This file contains the implementation of evaluation components.
// The header (eval.hpp) contains declarations and constant tables.
// ============================================================================

#include "eval.hpp"
#include "tuning.hpp"

namespace Eval {

PawnTable pawnTable;

// ============================================================================
// Helper Functions Implementation
// ============================================================================

Square flip_square(Square sq) {
    return Square(sq ^ 56);
}

Bitboard file_bb(File f) {
    return 0x0101010101010101ULL << f;
}

Bitboard adjacent_files_bb(File f) {
    Bitboard result = 0;
    if (f > FILE_A) result |= file_bb(File(f - 1));
    if (f < FILE_H) result |= file_bb(File(f + 1));
    return result;
}

Bitboard rank_bb_eval(Rank r) {
    return 0xFFULL << (r * 8);
}

Bitboard pawn_front_span(Color c, Square s) {
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

Bitboard passed_pawn_mask(Color c, Square s) {
    Bitboard front = pawn_front_span(c, s);
    File f = file_of(s);
    if (f > FILE_A) front |= pawn_front_span(c, Square(s - 1));
    if (f < FILE_H) front |= pawn_front_span(c, Square(s + 1));
    return front;
}

Square pawn_push(Color c, Square s) {
    return c == WHITE ? Square(s + 8) : Square(s - 8);
}

bool is_backward_pawn(Color c, Square s, Bitboard ourPawns, Bitboard theirPawns) {
    File f = file_of(s);
    Rank r = rank_of(s);

    // Get friendly pawns on adjacent files
    Bitboard neighbors = adjacent_files_bb(f) & ourPawns;
    if (!neighbors) return false;  // Isolated pawns handled separately

    // Check if all neighboring pawns are ahead of us
    Bitboard behindMask = c == WHITE ?
        (0xFFFFFFFFFFFFFFFFULL >> (64 - r * 8)) :
        (0xFFFFFFFFFFFFFFFFULL << ((r + 1) * 8));

    // If no neighbor is behind or on same rank, we might be backward
    if (neighbors & ~behindMask) return false;  // Some neighbor can still support us

    // Check if the stop square is attacked by enemy pawns
    Square stopSq = pawn_push(c, s);
    Bitboard stopAttacks = c == WHITE ?
        ((square_bb(stopSq) >> 7) & ~file_bb(FILE_A)) | ((square_bb(stopSq) >> 9) & ~file_bb(FILE_H)) :
        ((square_bb(stopSq) << 7) & ~file_bb(FILE_H)) | ((square_bb(stopSq) << 9) & ~file_bb(FILE_A));

    return (stopAttacks & theirPawns) != 0;
}

// ============================================================================
// Evaluation Components Implementation
// ============================================================================

EvalScore eval_material_pst(const Board& board, Color c) {
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

EvalScore eval_pawn_structure(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    // First pass: identify all passed pawns for connected passed pawn detection
    Bitboard passedPawns = 0;
    Bitboard tempBb = ourPawns;
    while (tempBb) {
        Square sq = pop_lsb(tempBb);
        if (!(passed_pawn_mask(c, sq) & theirPawns)) {
            passedPawns |= square_bb(sq);
        }
    }

    Bitboard bb = ourPawns;
    while (bb) {
        Square sq = pop_lsb(bb);
        File f = file_of(sq);
        Rank r = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        bool isIsolated = !(adjacent_files_bb(f) & ourPawns);

        // Passed pawn check
        bool isPassed = (passedPawns & square_bb(sq)) != 0;
        if (isPassed) {
            score += PassedPawnBonus[r];

            // Connected passed pawn bonus
            Bitboard adjacentPassers = adjacent_files_bb(f) & passedPawns;
            if (adjacentPassers) {
                Bitboard supportRange = rank_bb_eval(rank_of(sq));
                if (rank_of(sq) > RANK_1) supportRange |= rank_bb_eval(Rank(rank_of(sq) - 1));
                if (rank_of(sq) < RANK_8) supportRange |= rank_bb_eval(Rank(rank_of(sq) + 1));

                if (adjacentPassers & supportRange) {
                    score += ConnectedPassedBonus[r];
                }
            }
        }

        // Isolated pawn check
        if (isIsolated) {
            score += IsolatedPawnPenalty;
        }

        // Doubled pawn check
        if (popcount(file_bb(f) & ourPawns) > 1) {
            score += DoubledPawnPenalty;
        }

        // Backward pawn check (only if not isolated - isolated already penalized)
        if (!isIsolated && !isPassed && is_backward_pawn(c, sq, ourPawns, theirPawns)) {
            score += BackwardPawnPenalty;
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

EvalScore eval_pieces(const Board& board, Color c) {
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

EvalScore eval_king_safety(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Square kingSq = board.king_square(c);
    Bitboard occupied = board.pieces();
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    // King zone (3x3 area around king)
    Bitboard kingZone = king_attacks_bb(kingSq) | square_bb(kingSq);

    int attackUnits = 0;
    int attackCount = 0;

    // Count attacks by piece type
    Bitboard knightAttackers = board.pieces(enemy, KNIGHT);
    while (knightAttackers) {
        Square sq = pop_lsb(knightAttackers);
        if (knight_attacks_bb(sq) & kingZone) {
            attackUnits += KnightAttackWeight;
            attackCount++;
        }
    }

    // Sliders
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
        // Apply tunable weight (percentage)
        safetyPenalty = safetyPenalty * Tuning::KingSafetyWeight / 100;
        score.mg -= safetyPenalty;
    }

    // Semi-open and open file evaluation near enemy king
    Square enemyKingSq = board.king_square(enemy);
    File enemyKingFile = file_of(enemyKingSq);

    for (int df = -1; df <= 1; df++) {
        File f = File(enemyKingFile + df);
        if (f < FILE_A || f > FILE_H) continue;

        Bitboard fileMask = file_bb(f);
        bool hasOurPawn = (fileMask & ourPawns) != 0;
        bool hasTheirPawn = (fileMask & theirPawns) != 0;

        if (!hasOurPawn) {
            if (!hasTheirPawn) {
                score += KingOpenFilePenalty;
            } else {
                score += KingSemiOpenFilePenalty;
            }
        }
    }

    // Pawn shield evaluation
    Rank kingRank = c == WHITE ? rank_of(kingSq) : Rank(RANK_8 - rank_of(kingSq));
    if (kingRank <= RANK_2) {
        File kingFile = file_of(kingSq);
        Bitboard shieldZone = c == WHITE ?
            (rank_bb_eval(RANK_2) | rank_bb_eval(RANK_3)) :
            (rank_bb_eval(RANK_6) | rank_bb_eval(RANK_7));

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

// Lazy evaluation margin - if material+PST+pawn score is this far from
// alpha/beta, skip expensive calculations (mobility, king safety)
constexpr int LAZY_MARGIN = 500;

int evaluate(const Board& board, int alpha, int beta) {
    EvalScore score;

    // Calculate game phase
    int phase = TotalPhase;
    phase -= popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase -= popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase -= popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase -= popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::max(0, phase);

    // Material and PST - use incrementally updated scores (no loop needed!)
    score += board.psqt_score(WHITE);
    score -= board.psqt_score(BLACK);

    // Pawn structure (relatively cheap with hash table)
    Key pawnKey = board.pawn_key();
    PawnEntry* pawnEntry = pawnTable.probe(pawnKey);
    EvalScore pawnScore;

    if (pawnEntry->match(pawnKey)) {
        pawnScore = pawnEntry->score;
    } else {
        pawnScore += eval_pawn_structure(board, WHITE);
        pawnScore -= eval_pawn_structure(board, BLACK);

        pawnEntry->key = pawnKey;
        pawnEntry->score = pawnScore;
    }
    score += pawnScore;

    // Lazy evaluation check - compute approximate score with current data
    {
        int mg = score.mg;
        int eg = score.eg;
        int lazyScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;
        lazyScore = board.side_to_move() == WHITE ? lazyScore : -lazyScore;

        // If score is far above beta or far below alpha, skip expensive calculations
        if (lazyScore >= beta + LAZY_MARGIN || lazyScore <= alpha - LAZY_MARGIN) {
            return lazyScore;
        }
    }

    // Expensive calculations - only computed if lazy eval didn't cut off
    // Piece activity (mobility)
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

// Overload without alpha/beta for compatibility (no lazy eval)
int evaluate(const Board& board) {
    return evaluate(board, -30000, 30000);
}

} // namespace Eval

