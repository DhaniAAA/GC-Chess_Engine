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
// EvalContext Initialization (CPW Attack and Defend Maps)
// ============================================================================

void init_eval_context(EvalContext& ctx, const Board& board) {
    ctx.clear();
    Bitboard occupied = board.pieces();

    for (Color c : {WHITE, BLACK}) {
        Color enemy = ~c;
        ctx.kingSquare[c] = board.king_square(c);

        // Pawn attacks
        Bitboard ourPawns = board.pieces(c, PAWN);
        ctx.attackedBy[c][PAWN] = pawn_attacks_bb(c, ourPawns);
        ctx.attackedBy[c][ALL_PIECES] = ctx.attackedBy[c][PAWN];

        // King ring
        Square kingSq = ctx.kingSquare[c];
        ctx.kingRing[c] = king_attacks_bb(kingSq) | square_bb(kingSq);
        if (c == WHITE) {
            ctx.kingRing[c] |= (ctx.kingRing[c] << 8) & ~RANK_8_BB;
        } else {
            ctx.kingRing[c] |= (ctx.kingRing[c] >> 8) & ~RANK_1_BB;
        }

        // King attacks
        ctx.attackedBy[c][KING] = king_attacks_bb(kingSq);
        ctx.attackedBy2[c] = ctx.attackedBy[c][PAWN] & ctx.attackedBy[c][KING];
        ctx.attackedBy[c][ALL_PIECES] |= ctx.attackedBy[c][KING];

        // Mobility area
        Bitboard enemyPawnAttacks = pawn_attacks_bb(enemy, board.pieces(enemy, PAWN));
        ctx.mobilityArea[c] = ~(board.pieces(c) | enemyPawnAttacks);
    }

    // Piece attacks
    for (Color c : {WHITE, BLACK}) {
        Color enemy = ~c;

        // Knights
        Bitboard knights = board.pieces(c, KNIGHT);
        Bitboard knightAttacks = EMPTY_BB;
        while (knights) {
            Square sq = pop_lsb(knights);
            Bitboard attacks = knight_attacks_bb(sq);
            knightAttacks |= attacks;
            ctx.attackedBy2[c] |= ctx.attackedBy[c][ALL_PIECES] & attacks;
            ctx.attackedBy[c][ALL_PIECES] |= attacks;
            if (attacks & ctx.kingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += KnightAttackWeight;
            }
        }
        ctx.attackedBy[c][KNIGHT] = knightAttacks;

        // Bishops
        Bitboard bishops = board.pieces(c, BISHOP);
        Bitboard bishopAttacks = EMPTY_BB;
        while (bishops) {
            Square sq = pop_lsb(bishops);
            Bitboard attacks = bishop_attacks_bb(sq, occupied);
            bishopAttacks |= attacks;
            ctx.attackedBy2[c] |= ctx.attackedBy[c][ALL_PIECES] & attacks;
            ctx.attackedBy[c][ALL_PIECES] |= attacks;
            if (attacks & ctx.kingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += BishopAttackWeight;
            }
        }
        ctx.attackedBy[c][BISHOP] = bishopAttacks;

        // Rooks
        Bitboard rooks = board.pieces(c, ROOK);
        Bitboard rookAttacks = EMPTY_BB;
        while (rooks) {
            Square sq = pop_lsb(rooks);
            Bitboard attacks = rook_attacks_bb(sq, occupied);
            rookAttacks |= attacks;
            ctx.attackedBy2[c] |= ctx.attackedBy[c][ALL_PIECES] & attacks;
            ctx.attackedBy[c][ALL_PIECES] |= attacks;
            if (attacks & ctx.kingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += RookAttackWeight;
            }
        }
        ctx.attackedBy[c][ROOK] = rookAttacks;

        // Queens
        Bitboard queens = board.pieces(c, QUEEN);
        Bitboard queenAttacks = EMPTY_BB;
        while (queens) {
            Square sq = pop_lsb(queens);
            Bitboard attacks = queen_attacks_bb(sq, occupied);
            queenAttacks |= attacks;
            ctx.attackedBy2[c] |= ctx.attackedBy[c][ALL_PIECES] & attacks;
            ctx.attackedBy[c][ALL_PIECES] |= attacks;
            if (attacks & ctx.kingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += QueenAttackWeight;
            }
        }
        ctx.attackedBy[c][QUEEN] = queenAttacks;
    }
    ctx.initialized = true;
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
    Bitboard ourRooks = board.pieces(c, ROOK);
    Square ourKingSq = board.king_square(c);
    Square enemyKingSq = board.king_square(enemy);


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

            // King proximity to passed pawn (important in endgame)
            int ourKingDist = std::max(std::abs(file_of(ourKingSq) - f),
                                       std::abs(rank_of(ourKingSq) - rank_of(sq)));
            int enemyKingDist = std::max(std::abs(file_of(enemyKingSq) - f),
                                         std::abs(rank_of(enemyKingSq) - rank_of(sq)));
            // In endgame: bonus if our king is close, penalty if enemy king is close
            score.eg += (enemyKingDist - ourKingDist) * 5;  // Up to +/- 35 for max distance diff

            // Rook behind passed pawn bonus
            // Check if rook is behind (opposite direction from promotion)
            // Invert: check if rook is behind (opposite direction from promotion)
            Bitboard behindPawn = c == WHITE ?
                (file_bb(f) & (square_bb(sq) - 1)) :  // Squares below for white
                (file_bb(f) & ~(square_bb(sq) | (square_bb(sq) - 1)));  // Squares above for black
            if (behindPawn & ourRooks) {
                score.mg += 15;
                score.eg += 25;  // More valuable in endgame
            }

            // Blockaded passed pawn penalty
            Square stopSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
            if (stopSq >= SQ_A1 && stopSq <= SQ_H8) {
                Piece blocker = board.piece_on(stopSq);
                if (blocker != NO_PIECE && color_of(blocker) == enemy) {
                    // Passed pawn is blocked
                    score.mg -= 10;
                    score.eg -= 20;  // More significant in endgame
                }
            }

            // =================================================================
            // RULE OF THE SQUARE (Endgame Only)
            // A passed pawn that the enemy king cannot catch is virtually unstoppable
            // The "square" is the area the enemy king must be in to catch the pawn
            // =================================================================
            {
                // Calculate promotion square
                Rank promoRank = c == WHITE ? RANK_8 : RANK_1;
                int squaresToPromo = std::abs(promoRank - rank_of(sq));

                // For the pawn to move, we need to check if it's the pawn's turn
                int effectiveSquares = squaresToPromo;
                if (board.side_to_move() != c) {
                    // If it's enemy's turn, pawn effectively has one less square
                    effectiveSquares++;
                }

                // Enemy king distance to the promotion square
                int kingToPromo = std::max(
                    std::abs(file_of(enemyKingSq) - f),
                    std::abs(rank_of(enemyKingSq) - promoRank)
                );

                // Check if enemy king is outside the "square"
                // King is outside if its Chebyshev distance to promo square > effective squares
                if (kingToPromo > effectiveSquares) {
                    // Check if path is clear (no enemy pieces blocking)
                    bool pathClear = true;
                    Bitboard enemyPieces = board.pieces(enemy) ^ board.pieces(enemy, KING);
                    Bitboard pawnPath = c == WHITE ?
                        (file_bb(f) & ~(square_bb(sq) | (square_bb(sq) - 1))) :  // Above pawn for white
                        (file_bb(f) & (square_bb(sq) - 1));  // Below pawn for black

                    if (pawnPath & enemyPieces) {
                        pathClear = false;
                    }

                    if (pathClear) {
                        // Enemy king cannot catch the pawn!
                        score.eg += RuleOfSquareBonus;
                    }
                }
            }
        }
        // =================================================================
        // CANDIDATE PASSED PAWN
        // A pawn that could become passed if we advance it and exchange
        // or if enemy pawn blocking it leaves. Has potential but needs work.
        // =================================================================
        else {
            // Check if pawn could become a candidate passed pawn
            // Candidate: only ONE enemy pawn can stop it, and we have a helper pawn
            Bitboard stoppers = passed_pawn_mask(c, sq) & theirPawns;
            int stopperCount = popcount(stoppers);

            if (stopperCount == 1) {
                // Only one enemy pawn blocking - this is a candidate
                // (stopperSq not needed - we just need to know there's exactly one stopper)

                // Check if we have a helper pawn that can support the exchange
                Bitboard helperMask = adjacent_files_bb(f) & ourPawns;
                bool hasHelper = false;

                if (c == WHITE) {
                    // Helper pawns must be behind or equal rank to attack the stopper
                    Bitboard behindOrEqual = ~0ULL >> (8 * (8 - rank_of(sq)));
                    if (helperMask & behindOrEqual) hasHelper = true;
                } else {
                    Bitboard behindOrEqual = ~0ULL << (8 * (rank_of(sq) + 1));
                    if (helperMask & behindOrEqual) hasHelper = true;
                }

                // Also consider if our pawn can simply capture the stopper
                Bitboard pawnAttacks = pawn_attacks_bb(c, sq);
                if (pawnAttacks & stoppers) {
                    hasHelper = true;  // Can directly capture
                }

                if (hasHelper) {
                    score += CandidatePassedBonus[r];
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
    Square enemyKingSq = board.king_square(enemy);

    // Knights
    Bitboard bb = board.pieces(c, KNIGHT);
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = knight_attacks_bb(sq);
        int mobility = popcount(attacks & mobilityArea);
        score += KnightMobility[std::min(mobility, 8)];

        // Outpost check - knight defended by OUR pawns
        // BUG FIX: Was using pawn_attacks_bb(enemy, ourPawns) which is WRONG
        // Correct: pawn_attacks_bb(c, ourPawns) = squares defended by our pawns
        Bitboard pawnDefenders = pawn_attacks_bb(c, ourPawns);
        if ((square_bb(sq) & pawnDefenders)) {
            Rank relativeRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
            if (relativeRank >= RANK_4 && relativeRank <= RANK_6) {
                score += KnightOutpostBonus;
            }
        }

        // Knight tropism - bonus for knights close to enemy king
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        // Closer = better, max bonus at distance 1-2
        score.mg += std::max(0, (5 - kingDist) * 3);  // Up to 12cp for adjacent squares
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

        // Bad bishop detection - bishop blocked by own pawns on same color squares
        bool isLightSquare = ((file_of(sq) + rank_of(sq)) % 2) == 1;
        Bitboard sameColorSquares = isLightSquare ?
            0x55AA55AA55AA55AAULL : 0xAA55AA55AA55AA55ULL;  // Light or dark squares
        int blockedPawns = popcount(ourPawns & sameColorSquares);
        // Penalize if many own pawns on same color as bishop
        if (blockedPawns >= 4) {
            score.mg -= (blockedPawns - 3) * 8;   // -8 to -40 for 4-8 pawns
            score.eg -= (blockedPawns - 3) * 5;   // Less penalty in endgame
        }
    }
    // Bishop pair bonus
    if (bishopCount >= 2) {
        score += BishopPairBonus;
    }

    // Rooks
    bb = board.pieces(c, ROOK);
    Square rookSquares[2] = { SQ_NONE, SQ_NONE };
    int rookCount = 0;
    while (bb) {
        Square sq = pop_lsb(bb);
        if (rookCount < 2) {
            rookSquares[rookCount] = sq;
        }
        rookCount++;

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

    // Connected rooks bonus - rooks on same rank/file with no pieces between
    if (rookCount >= 2 && rookSquares[0] != SQ_NONE && rookSquares[1] != SQ_NONE) {
        if (file_of(rookSquares[0]) == file_of(rookSquares[1]) ||
            rank_of(rookSquares[0]) == rank_of(rookSquares[1])) {
            // Check if they can see each other
            Bitboard between = between_bb(rookSquares[0], rookSquares[1]);
            if (!(between & occupied)) {
                score.mg += 15;  // Connected rooks bonus
                score.eg += 10;
            }
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

// ============================================================================
// Space Evaluation
// Evaluates control of central squares behind pawns.
// More space = more room for pieces to maneuver.
// ============================================================================

EvalScore eval_space(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;

    // Space area: central squares (files C-F, ranks 2-4 for White, 5-7 for Black)
    constexpr Bitboard WhiteSpaceArea = 0x00003C3C3C000000ULL;  // C2-F4 extended
    constexpr Bitboard BlackSpaceArea = 0x000000003C3C3C00ULL;  // C5-F7 extended

    Bitboard spaceArea = (c == WHITE) ? WhiteSpaceArea : BlackSpaceArea;

    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    // Calculate "behind pawns" area - squares our pawns protect from the rear
    Bitboard behindPawns;
    if (c == WHITE) {
        // All squares south of our pawns
        behindPawns = ourPawns >> 8;
        behindPawns |= behindPawns >> 8;
        behindPawns |= behindPawns >> 16;
    } else {
        // All squares north of our pawns
        behindPawns = ourPawns << 8;
        behindPawns |= behindPawns << 8;
        behindPawns |= behindPawns << 16;
    }

    // Safe squares = in space area AND (behind our pawns OR not attacked by enemy pawns)
    Bitboard enemyPawnAttacks = pawn_attacks_bb(enemy, theirPawns);
    Bitboard safe = spaceArea & (behindPawns | ~enemyPawnAttacks);

    // Count safe space squares
    int spaceCount = popcount(safe);

    // Bonus scales with piece count (space matters more with many pieces)
    int pieceCount = popcount(board.pieces(c)) - 1;  // Exclude king
    int spaceBonus = (spaceCount * spaceCount * pieceCount) / 128;

    score.mg = spaceBonus;
    score.eg = spaceBonus / 2;  // Less important in endgame

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
// EvalContext-based Evaluation Functions
// ============================================================================

EvalScore eval_threats_with_context(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;

    // Hanging pieces
    Bitboard attacked = ctx.attackedBy[c][ALL_PIECES];
    Bitboard enemyPieces = board.pieces(enemy);
    Bitboard defended = ctx.attackedBy[enemy][ALL_PIECES];
    Bitboard hanging = enemyPieces & attacked & ~defended;
    score.mg += popcount(hanging) * 25;
    score.eg += popcount(hanging) * 20;

    // Minors attacked by pawns
    Bitboard attackedByPawn = ctx.attackedBy[c][PAWN];
    Bitboard minorsAttacked = (board.pieces(enemy, KNIGHT) | board.pieces(enemy, BISHOP)) & attackedByPawn;
    score.mg += popcount(minorsAttacked) * 40;
    score.eg += popcount(minorsAttacked) * 30;

    // Rooks attacked by minors
    Bitboard rooksAttacked = board.pieces(enemy, ROOK) &
        (ctx.attackedBy[c][KNIGHT] | ctx.attackedBy[c][BISHOP]);
    score.mg += popcount(rooksAttacked) * 35;
    score.eg += popcount(rooksAttacked) * 25;

    // Double attack advantage
    Bitboard contested = ctx.attackedBy[c][ALL_PIECES] & ctx.attackedBy[enemy][ALL_PIECES];
    Bitboard ourDouble = contested & ctx.attackedBy2[c];
    Bitboard theirSingle = contested & ~ctx.attackedBy2[enemy];
    Bitboard advantage = ourDouble & theirSingle;
    score.mg += popcount(advantage) * 8;
    score.eg += popcount(advantage) * 5;

    return score;
}

EvalScore eval_pieces_with_context(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard occupied = board.pieces();
    Square enemyKingSq = ctx.kingSquare[enemy];
    Bitboard mobilityArea = ctx.mobilityArea[c];
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    // Knights
    Bitboard bb = board.pieces(c, KNIGHT);
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = knight_attacks_bb(sq);
        int mobility = popcount(attacks & mobilityArea);
        score += KnightMobility[std::min(mobility, 8)];

        // Outpost
        if (ctx.attackedBy[c][PAWN] & square_bb(sq)) {
            Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
            if (relRank >= RANK_4 && relRank <= RANK_6) {
                score += KnightOutpostBonus;
            }
        }

        // Tropism
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += std::max(0, (5 - kingDist) * 3);
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

        // Bad bishop
        bool isLight = ((file_of(sq) + rank_of(sq)) % 2) == 1;
        Bitboard sameColor = isLight ? 0x55AA55AA55AA55AAULL : 0xAA55AA55AA55AA55ULL;
        int blocked = popcount(ourPawns & sameColor);
        if (blocked >= 4) {
            score.mg -= (blocked - 3) * 8;
            score.eg -= (blocked - 3) * 5;
        }
    }
    if (bishopCount >= 2) score += BishopPairBonus;

    // Rooks
    bb = board.pieces(c, ROOK);
    Square rookSquares[2] = {SQ_NONE, SQ_NONE};
    int rookCount = 0;
    while (bb) {
        Square sq = pop_lsb(bb);
        if (rookCount < 2) rookSquares[rookCount] = sq;
        rookCount++;

        File f = file_of(sq);
        Bitboard attacks = rook_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += RookMobility[std::min(mobility, 14)];

        // Open file
        Bitboard filePawns = file_bb(f);
        if (!(filePawns & ourPawns)) {
            if (!(filePawns & theirPawns)) score += RookOpenFileBonus;
            else score += RookSemiOpenFileBonus;
        }

        // 7th rank
        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        if (relRank == RANK_7) score += RookOnSeventhBonus;
    }

    // Connected rooks
    if (rookCount >= 2 && rookSquares[0] != SQ_NONE && rookSquares[1] != SQ_NONE) {
        if (file_of(rookSquares[0]) == file_of(rookSquares[1]) ||
            rank_of(rookSquares[0]) == rank_of(rookSquares[1])) {
            Bitboard between = between_bb(rookSquares[0], rookSquares[1]);
            if (!(between & occupied)) {
                score.mg += 15;
                score.eg += 10;
            }
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

    // Trapped pieces
    bb = board.pieces(c, BISHOP);
    while (bb) {
        Square sq = pop_lsb(bb);
        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        File f = file_of(sq);
        if (relRank == RANK_7) {
            if (f == FILE_A) {
                Square block = c == WHITE ? SQ_B6 : SQ_B3;
                if (theirPawns & square_bb(block)) { score.mg -= 100; score.eg -= 80; }
            } else if (f == FILE_H) {
                Square block = c == WHITE ? SQ_G6 : SQ_G3;
                if (theirPawns & square_bb(block)) { score.mg -= 100; score.eg -= 80; }
            }
        }
    }

    return score;
}

EvalScore eval_king_safety_with_context(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;
    Square kingSq = ctx.kingSquare[c];
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    int attackUnits = ctx.kingAttackersWeight[enemy];
    int attackCount = ctx.kingAttackersCount[enemy];

    // Virtual mobility
    Bitboard virtualQ = queen_attacks_bb(kingSq, board.pieces());
    Bitboard enemySliders = board.pieces(enemy, BISHOP, QUEEN) | board.pieces(enemy, ROOK);
    attackUnits += popcount(virtualQ & enemySliders);

    // Safe checks
    Bitboard unsafeFor = ctx.attackedBy[c][ALL_PIECES];
    Bitboard safeSquares = ~unsafeFor & ~board.pieces(enemy);

    if (ctx.attackedBy[enemy][KNIGHT] & (knight_attacks_bb(kingSq) & safeSquares)) attackUnits += 3;
    if (ctx.attackedBy[enemy][BISHOP] & (bishop_attacks_bb(kingSq, board.pieces()) & safeSquares)) attackUnits += 2;
    if (ctx.attackedBy[enemy][ROOK] & (rook_attacks_bb(kingSq, board.pieces()) & safeSquares)) attackUnits += 3;
    if (ctx.attackedBy[enemy][QUEEN] & (king_attacks_bb(kingSq) & safeSquares)) attackUnits += 6;

    if (attackCount >= 2) {
        int penalty = KingSafetyTable[std::min(attackUnits, 99)];
        penalty = penalty * Tuning::KingSafetyWeight / 100;
        score.mg -= penalty;
    }

    // Pawn shield
    Rank kingRank = c == WHITE ? rank_of(kingSq) : Rank(RANK_8 - rank_of(kingSq));
    if (kingRank <= RANK_2) {
        File kf = file_of(kingSq);
        Bitboard shieldZone = c == WHITE ?
            (rank_bb_eval(RANK_2) | rank_bb_eval(RANK_3)) :
            (rank_bb_eval(RANK_6) | rank_bb_eval(RANK_7));
        Bitboard shieldMask = 0;
        if (kf > FILE_A) shieldMask |= file_bb(File(kf - 1));
        shieldMask |= file_bb(kf);
        if (kf < FILE_H) shieldMask |= file_bb(File(kf + 1));
        int shieldCount = popcount(shieldMask & shieldZone & ourPawns);
        score.mg += PawnShieldBonus[std::min(shieldCount, 3)];
    }

    // Open files near OUR king (penalty)
    File ourKingFile = file_of(kingSq);
    for (int df = -1; df <= 1; df++) {
        File f = File(ourKingFile + df);
        if (f < FILE_A || f > FILE_H) continue;
        Bitboard fileMask = file_bb(f);
        if (!(fileMask & ourPawns)) {
            if (!(fileMask & theirPawns)) { score.mg -= 25; score.eg -= 10; }
            else { score.mg -= 15; score.eg -= 5; }
        }
    }

    // Pawn storm
    Square enemyKingSq = ctx.kingSquare[enemy];
    File enemyKingFile = file_of(enemyKingSq);
    Rank enemyKingRank = enemy == WHITE ? rank_of(enemyKingSq) : Rank(RANK_8 - rank_of(enemyKingSq));
    if (enemyKingRank <= RANK_2) {
        int stormBonus = 0;
        for (int df = -1; df <= 1; df++) {
            File f = File(enemyKingFile + df);
            if (f < FILE_A || f > FILE_H) continue;
            Bitboard filePawns = file_bb(f) & ourPawns;
            if (filePawns) {
                Square advSq = c == WHITE ? msb(filePawns) : lsb(filePawns);
                Rank advRank = c == WHITE ? rank_of(advSq) : Rank(RANK_8 - rank_of(advSq));
                if (advRank >= RANK_5) stormBonus += (advRank - RANK_4) * 5;
            }
        }
        score.mg += stormBonus;
    }

    return score;
}

// ============================================================================
// Main Evaluation Function
// ============================================================================

// Lazy evaluation margin - if material+PST+pawn score is this far from
// alpha/beta, skip expensive calculations (mobility, king safety)
// Using a moderate margin (500cp ~= 5 pawns) to balance speed and accuracy
// Too high a margin causes missed tactical evaluations
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

    // Initialize EvalContext (CPW Attack Maps)
    EvalContext ctx;
    init_eval_context(ctx, board);

    // Piece activity using cached attack maps
    score += eval_pieces_with_context(board, WHITE, ctx);
    score -= eval_pieces_with_context(board, BLACK, ctx);

    // King safety using cached attack maps
    score += eval_king_safety_with_context(board, WHITE, ctx);
    score -= eval_king_safety_with_context(board, BLACK, ctx);

    // Threat evaluation using cached attack maps
    score += eval_threats_with_context(board, WHITE, ctx);
    score -= eval_threats_with_context(board, BLACK, ctx);

    // Space evaluation (control of central squares)
    score += eval_space(board, WHITE);
    score -= eval_space(board, BLACK);

    // Tapered evaluation
    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    // Tempo bonus - small bonus for side to move (having the initiative)
    constexpr int TEMPO = 15;  // About 0.15 pawn

    // Return from side to move's perspective with tempo
    return (board.side_to_move() == WHITE ? finalScore : -finalScore) + TEMPO;
}

// Overload without alpha/beta for compatibility (no lazy eval)
int evaluate(const Board& board) {
    return evaluate(board, -30000, 30000);
}

// Thread-safe version without pawn hash table (for Texel Tuner multi-threading)
// This avoids race conditions when multiple threads evaluate positions simultaneously
int evaluate_no_cache(const Board& board) {
    EvalScore score;

    // Calculate game phase
    int phase = TotalPhase;
    phase -= popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase -= popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase -= popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase -= popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::max(0, phase);

    // Material and PST - use incrementally updated scores
    score += board.psqt_score(WHITE);
    score -= board.psqt_score(BLACK);

    // Pawn structure (computed directly, no caching)
    EvalScore pawnScore;
    pawnScore += eval_pawn_structure(board, WHITE);
    pawnScore -= eval_pawn_structure(board, BLACK);
    score += pawnScore;

    // Initialize EvalContext
    EvalContext ctx;
    init_eval_context(ctx, board);

    // Piece activity
    score += eval_pieces_with_context(board, WHITE, ctx);
    score -= eval_pieces_with_context(board, BLACK, ctx);

    // King safety
    score += eval_king_safety_with_context(board, WHITE, ctx);
    score -= eval_king_safety_with_context(board, BLACK, ctx);

    // Threats
    score += eval_threats_with_context(board, WHITE, ctx);
    score -= eval_threats_with_context(board, BLACK, ctx);

    // Space evaluation
    score += eval_space(board, WHITE);
    score -= eval_space(board, BLACK);

    // Tapered evaluation
    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    // Tempo bonus
    constexpr int TEMPO = 15;

    // Return from side to move's perspective with tempo
    return (board.side_to_move() == WHITE ? finalScore : -finalScore) + TEMPO;
}

// ============================================================================
// Material Balance
// Returns material difference from White's perspective in centipawns
// Positive = White ahead, Negative = Black ahead
// Used for dynamic contempt calculation
// ============================================================================

int material_balance(const Board& board) {
    int balance = 0;

    // Count material for each side
    balance += popcount(board.pieces(WHITE, PAWN)) * PawnValue.mg;
    balance -= popcount(board.pieces(BLACK, PAWN)) * PawnValue.mg;

    balance += popcount(board.pieces(WHITE, KNIGHT)) * KnightValue.mg;
    balance -= popcount(board.pieces(BLACK, KNIGHT)) * KnightValue.mg;

    balance += popcount(board.pieces(WHITE, BISHOP)) * BishopValue.mg;
    balance -= popcount(board.pieces(BLACK, BISHOP)) * BishopValue.mg;

    balance += popcount(board.pieces(WHITE, ROOK)) * RookValue.mg;
    balance -= popcount(board.pieces(BLACK, ROOK)) * RookValue.mg;

    balance += popcount(board.pieces(WHITE, QUEEN)) * QueenValue.mg;
    balance -= popcount(board.pieces(BLACK, QUEEN)) * QueenValue.mg;

    return balance;
}

} // namespace Eval

