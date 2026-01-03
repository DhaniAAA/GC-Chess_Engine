// ============================================================================
// eval.cpp - Evaluation Function Implementation
// ============================================================================
// This file contains the implementation of evaluation components.
// The header (eval.hpp) contains declarations and constant tables.
// ============================================================================

#include "eval.hpp"
#include "tuning.hpp"
#include "profiler.hpp"

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
// Centralization Helper Functions
// ============================================================================

// Returns centralization index (0-3) for a square
// 0 = edge/corner, 1 = outer ring, 2 = inner ring, 3 = center
int get_centralization_index(Square sq) {
    File f = file_of(sq);
    Rank r = rank_of(sq);

    // Distance from center (d4, d5, e4, e5)
    int fileDistFromCenter = std::min(std::abs(f - FILE_D), std::abs(f - FILE_E));
    int rankDistFromCenter = std::min(std::abs(r - RANK_4), std::abs(r - RANK_5));
    int maxDist = std::max(fileDistFromCenter, rankDistFromCenter);

    // Map distance to centralization index
    // 0 distance = center (index 3)
    // 1 distance = inner ring (index 2)
    // 2 distance = outer ring (index 1)
    // 3+ distance = edge (index 0)
    if (maxDist == 0) return 3;  // Center squares (d4, d5, e4, e5)
    if (maxDist == 1) return 2;  // Inner ring (c3-f6)
    if (maxDist == 2) return 1;  // Outer ring (b2-g7)
    return 0;  // Edge
}

// Returns file centralization index (0-3) for rooks
// Rooks prefer central files (d, e)
int get_file_centralization(File f) {
    if (f == FILE_D || f == FILE_E) return 3;  // Central files
    if (f == FILE_C || f == FILE_F) return 2;
    if (f == FILE_B || f == FILE_G) return 1;
    return 0;  // Edge files (a, h)
}

// Long diagonal masks
constexpr Bitboard LONG_DIAGONAL_A1H8 = 0x8040201008040201ULL;  // a1-h8
constexpr Bitboard LONG_DIAGONAL_H1A8 = 0x0102040810204080ULL;  // h1-a8

// ============================================================================
// EvalContext Initialization (CPW Attack and Defend Maps)
// ============================================================================

// Helper to compute outer king ring (squares 2 steps from king)
static Bitboard compute_outer_ring(Square kingSq) {
    Bitboard inner = king_attacks_bb(kingSq) | square_bb(kingSq);
    Bitboard outer = 0ULL;

    // Expand inner ring by one in each direction
    Bitboard temp = inner;
    while (temp) {
        Square sq = pop_lsb(temp);
        outer |= king_attacks_bb(sq);
    }

    // Outer ring is expanded area minus inner ring
    return outer & ~inner;
}

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

        // King rings - inner (immediately adjacent) and outer (2 squares away)
        Square kingSq = ctx.kingSquare[c];
        ctx.innerKingRing[c] = king_attacks_bb(kingSq) | square_bb(kingSq);
        ctx.outerKingRing[c] = compute_outer_ring(kingSq);

        // Combined king ring for compatibility (inner + forward extension)
        ctx.kingRing[c] = ctx.innerKingRing[c];
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

    // Piece attacks with separate inner/outer ring tracking
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

            // Track attacks on inner vs outer ring
            if (attacks & ctx.innerKingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += KnightAttackWeight * InnerRingAttackWeight;
                ctx.innerRingAttacks[c] += popcount(attacks & ctx.innerKingRing[enemy]);
            }
            if (attacks & ctx.outerKingRing[enemy]) {
                ctx.kingAttackersWeight[c] += KnightAttackWeight * OuterRingAttackWeight;
                ctx.outerRingAttacks[c] += popcount(attacks & ctx.outerKingRing[enemy]);
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

            if (attacks & ctx.innerKingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += BishopAttackWeight * InnerRingAttackWeight;
                ctx.innerRingAttacks[c] += popcount(attacks & ctx.innerKingRing[enemy]);
            }
            if (attacks & ctx.outerKingRing[enemy]) {
                ctx.kingAttackersWeight[c] += BishopAttackWeight * OuterRingAttackWeight;
                ctx.outerRingAttacks[c] += popcount(attacks & ctx.outerKingRing[enemy]);
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

            if (attacks & ctx.innerKingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += RookAttackWeight * InnerRingAttackWeight;
                ctx.innerRingAttacks[c] += popcount(attacks & ctx.innerKingRing[enemy]);
            }
            if (attacks & ctx.outerKingRing[enemy]) {
                ctx.kingAttackersWeight[c] += RookAttackWeight * OuterRingAttackWeight;
                ctx.outerRingAttacks[c] += popcount(attacks & ctx.outerKingRing[enemy]);
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

            if (attacks & ctx.innerKingRing[enemy]) {
                ctx.kingAttackersCount[c]++;
                ctx.kingAttackersWeight[c] += QueenAttackWeight * InnerRingAttackWeight;
                ctx.innerRingAttacks[c] += popcount(attacks & ctx.innerKingRing[enemy]);
            }
            if (attacks & ctx.outerKingRing[enemy]) {
                ctx.kingAttackersWeight[c] += QueenAttackWeight * OuterRingAttackWeight;
                ctx.outerRingAttacks[c] += popcount(attacks & ctx.outerKingRing[enemy]);
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

// Helper function to count pawn islands
// Pawn islands are groups of pawns on adjacent files
static int count_pawn_islands(Bitboard pawns) {
    if (!pawns) return 0;

    int islands = 0;
    bool inIsland = false;

    for (File f = FILE_A; f <= FILE_H; ++f) {
        bool hasFileOccupancy = (pawns & file_bb(f)) != 0;
        if (hasFileOccupancy && !inIsland) {
            islands++;
            inIsland = true;
        } else if (!hasFileOccupancy) {
            inIsland = false;
        }
    }

    return islands;
}

// Helper function to check if a square is a hole (cannot be defended by pawns)
static bool is_outpost_hole(Color c, Square sq, Bitboard ourPawns) {
    File f = file_of(sq);
    Rank r = rank_of(sq);

    // For white, check if any of our pawns on adjacent files can advance to defend this square
    // For black, the logic is mirrored
    Bitboard adjacentPawns = adjacent_files_bb(f) & ourPawns;
    if (!adjacentPawns) return true;  // No pawns to defend

    // Check if any pawn is behind this square and can defend it
    if (c == WHITE) {
        // Our pawns need to be on ranks below sq to defend it
        Bitboard behindMask = 0ULL;
        for (int rr = RANK_1; rr < r; ++rr) {
            behindMask |= rank_bb_eval(Rank(rr));
        }
        if (adjacentPawns & behindMask) return false;  // Can be defended
    } else {
        Bitboard behindMask = 0ULL;
        for (int rr = r + 1; rr <= RANK_8; ++rr) {
            behindMask |= rank_bb_eval(Rank(rr));
        }
        if (adjacentPawns & behindMask) return false;
    }

    return true;  // Is a hole
}

EvalScore eval_pawn_structure(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Bitboard ourRooks = board.pieces(c, ROOK);
    Square ourKingSq = board.king_square(c);
    Square enemyKingSq = board.king_square(enemy);

    // =========================================================================
    // Pawn Islands Evaluation
    // More islands = weaker structure (harder to defend)
    // =========================================================================
    int islands = count_pawn_islands(ourPawns);
    if (islands > 1) {
        score.mg += PawnIslandPenalty.mg * (islands - 1);
        score.eg += PawnIslandPenalty.eg * (islands - 1);
    }

    // =========================================================================
    // Central Pawn Bonus
    // Pawns on d4/e4 (for white) or d5/e5 (for black) are strong
    // =========================================================================
    constexpr Bitboard WhiteCentralSquares = (1ULL << SQ_D4) | (1ULL << SQ_E4);
    constexpr Bitboard BlackCentralSquares = (1ULL << SQ_D5) | (1ULL << SQ_E5);
    Bitboard centralSquares = c == WHITE ? WhiteCentralSquares : BlackCentralSquares;
    int centralPawns = popcount(ourPawns & centralSquares);
    score += CentralPawnBonus * centralPawns;


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

            // =================================================================
            // PROTECTED PASSED PAWN
            // A passed pawn defended by another pawn is extremely dangerous
            // It's harder to blockade and can often advance safely
            // =================================================================
            Bitboard pawnDefenders = pawn_attacks_bb(c, ourPawns);
            if (square_bb(sq) & pawnDefenders) {
                // Use ProtectedPassedBonus instead of regular PassedPawnBonus
                // We already added PassedPawnBonus, so add the difference
                score += ProtectedPassedBonus[r];
                score -= PassedPawnBonus[r];  // Remove base to avoid double counting
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

            // Extra penalty if backward pawn is on a half-open file
            Bitboard fileMask = file_bb(f);
            if (!(fileMask & theirPawns)) {
                score += BackwardOnHalfOpen;
            }
        }

        // Connected pawn check
        Bitboard adjacentPawns = adjacent_files_bb(f) & ourPawns;
        if (adjacentPawns) {
            // Check for phalanx (pawns on same rank)
            if (adjacentPawns & rank_bb_eval(rank_of(sq))) {
                score += PhalanxBonus;

                // Pawn duo bonus - side by side pawns
                score += PawnDuoBonus;
            } else {
                score += ConnectedPawnBonus;
            }

            // =================================================================
            // PAWN CHAIN DETECTION
            // A pawn is part of a chain if it's defended by another pawn
            // =================================================================
            Bitboard pawnDefenders = pawn_attacks_bb(c, ourPawns);
            if (square_bb(sq) & pawnDefenders) {
                score += PawnChainBonus;

                // Check if this pawn is the base of a chain (defends another pawn)
                Bitboard thisDefends = pawn_attacks_bb(c, sq);
                if (thisDefends & ourPawns) {
                    score += PawnChainBaseBonus;
                }
            }
        }

        // =================================================================
        // HANGING PAWNS DETECTION
        // Two pawns on adjacent semi-open files, no pawn support on either side
        // =================================================================
        if (!isIsolated && !isPassed) {
            // Check if this is part of a hanging pawn pair
            Bitboard leftFile = (f > FILE_A) ? file_bb(File(f - 1)) : 0;
            Bitboard rightFile = (f < FILE_H) ? file_bb(File(f + 1)) : 0;

            bool hasLeftNeighbor = (leftFile & ourPawns) != 0;
            bool hasRightNeighbor = (rightFile & ourPawns) != 0;

            // Check if exactly one neighbor and no wider support
            if (hasLeftNeighbor != hasRightNeighbor) {
                File neighborFile = hasLeftNeighbor ? File(f - 1) : File(f + 1);

                // Check if both files are semi-open (no enemy pawns)
                bool ourFileSemiOpen = !(file_bb(f) & theirPawns);
                bool neighborFileSemiOpen = !(file_bb(neighborFile) & theirPawns);

                if (ourFileSemiOpen && neighborFileSemiOpen) {
                    // Check for wider support
                    File outerFile = hasLeftNeighbor ? File(f - 2) : File(f + 2);
                    bool hasWiderSupport = false;
                    if (outerFile >= FILE_A && outerFile <= FILE_H) {
                        hasWiderSupport = (file_bb(outerFile) & ourPawns) != 0;
                    }

                    if (!hasWiderSupport) {
                        score += HangingPawnPenalty;

                        // Bonus if they can advance and create threats
                        Square advSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
                        if (advSq >= SQ_A1 && advSq <= SQ_H8) {
                            if (board.piece_on(advSq) == NO_PIECE) {
                                score += HangingPawnWithThreat;
                            }
                        }
                    }
                }
            }
        }
    }

    // =========================================================================
    // OUTPOST HOLES EVALUATION
    // Check for weak squares on ranks 4-6 that enemy pieces can occupy
    // =========================================================================
    constexpr Bitboard OutpostZoneWhite = 0x00FFFFFF00000000ULL;  // Ranks 5-7 for white
    constexpr Bitboard OutpostZoneBlack = 0x00000000FFFFFF00ULL;  // Ranks 2-4 for black
    Bitboard outpostZone = c == WHITE ? OutpostZoneWhite : OutpostZoneBlack;

    // Check central files (c-f) for holes
    for (File f = FILE_C; f <= FILE_F; ++f) {
        Bitboard fileZone = file_bb(f) & outpostZone;
        Bitboard temp = fileZone;
        while (temp) {
            Square sq = pop_lsb(temp);
            if (is_outpost_hole(c, sq, ourPawns)) {
                // Only penalize if the square is a real threat
                // (enemy can occupy it)
                score += OutpostHolePenalty;
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

    // =========================================================================
    // PIECE CONNECTIVITY - Bonus for pieces defended by other pieces
    // Well-connected pieces are harder to attack and provide mutual support
    // =========================================================================
    Bitboard ourPieces = board.pieces(c) & ~board.pieces(c, PAWN) & ~square_bb(board.king_square(c));
    Bitboard ourDefended = ctx.attackedBy[c][ALL_PIECES];

    // Count defended pieces (excluding pawns and king)
    Bitboard connectedPieces = ourPieces & ourDefended;
    int connectedCount = popcount(connectedPieces);

    // Bonus scales with number of connected pieces
    // Having 3+ connected pieces is very good
    if (connectedCount >= 3) {
        score.mg += (connectedCount - 2) * 8;
        score.eg += (connectedCount - 2) * 5;
    }

    // Penalty for undefended pieces (hanging pieces already counted above)
    // This is additional penalty for OUR undefended pieces
    Bitboard ourUndefended = ourPieces & ~ourDefended;
    int undefendedCount = popcount(ourUndefended);
    if (undefendedCount > 0) {
        score.mg -= undefendedCount * 12;
        score.eg -= undefendedCount * 8;
    }

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

    // Weak squares near king (undefended squares in king's vicinity)
    // Inner ring weak squares are more dangerous
    Bitboard ourDefended = ctx.attackedBy[c][ALL_PIECES];
    Bitboard innerWeak = ctx.innerKingRing[c] & ~ourDefended & ~board.pieces();
    Bitboard outerWeak = ctx.outerKingRing[c] & ~ourDefended & ~board.pieces();

    // Penalize weak squares, especially if enemy attacks them
    int innerWeakCount = popcount(innerWeak & ctx.attackedBy[enemy][ALL_PIECES]);
    int outerWeakCount = popcount(outerWeak & ctx.attackedBy[enemy][ALL_PIECES]);

    score.mg -= innerWeakCount * InnerRingWeakSquarePenalty;
    score.mg -= outerWeakCount * OuterRingWeakSquarePenalty;

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
// Material Imbalance Evaluation
// Evaluates bonuses/penalties for specific piece combinations
// ============================================================================

EvalScore eval_material_imbalance(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;

    int ourKnights = popcount(board.pieces(c, KNIGHT));
    int ourBishops = popcount(board.pieces(c, BISHOP));
    int ourRooks = popcount(board.pieces(c, ROOK));
    int ourQueens = popcount(board.pieces(c, QUEEN));

    int theirKnights = popcount(board.pieces(enemy, KNIGHT));
    int theirBishops = popcount(board.pieces(enemy, BISHOP));
    int theirRooks = popcount(board.pieces(enemy, ROOK));
    int theirQueens = popcount(board.pieces(enemy, QUEEN));

    // Rook pair bonus - two rooks work well together
    if (ourRooks >= 2) {
        score += RookPairBonus;
    }

    // Knight pair penalty - two knights are less synergistic
    if (ourKnights >= 2) {
        score += KnightPairPenalty;
    }

    // Bishop + Knight combo vs opponent's rook pair
    // Bishop and Knight complement each other well
    if (ourBishops >= 1 && ourKnights >= 1) {
        score += BishopKnightCombo;
    }

    // Rooks without queens are stronger (endgame power)
    if (ourRooks >= 1 && ourQueens == 0 && theirQueens == 0) {
        score += RooksWithoutQueens;
    }

    // Queen without rooks is weaker (less support)
    if (ourQueens >= 1 && ourRooks == 0) {
        score += QueenWithoutRooks;
    }

    // Minor for exchange compensation
    // If we have fewer rooks but more minors, partial compensation
    int ourMinors = ourKnights + ourBishops;
    int theirMinors = theirKnights + theirBishops;
    if (ourRooks < theirRooks && ourMinors > theirMinors) {
        int minorAdvantage = ourMinors - theirMinors;
        int rookDeficit = theirRooks - ourRooks;
        if (minorAdvantage >= rookDeficit) {
            score.eg += MinorForExchange.eg * std::min(minorAdvantage, 2);
        }
    }

    return score;
}

// ============================================================================
// Pawn Lever Evaluation
// Detects pawns that can attack enemy pawns to open files near enemy king
// ============================================================================

EvalScore eval_pawn_levers(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;

    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Square enemyKingSq = ctx.kingSquare[enemy];
    File enemyKingFile = file_of(enemyKingSq);

    // Only evaluate levers if enemy king is somewhat castled
    Rank enemyKingRank = enemy == WHITE ? rank_of(enemyKingSq) : Rank(RANK_8 - rank_of(enemyKingSq));
    if (enemyKingRank > RANK_2) return score;  // King not on back ranks

    // Check for pawn levers near enemy king
    Bitboard bb = ourPawns;
    while (bb) {
        Square sq = pop_lsb(bb);
        File pawnFile = file_of(sq);

        // Is this pawn near the enemy king's file?
        int fileDistance = std::abs(pawnFile - enemyKingFile);
        if (fileDistance > 2) continue;  // Too far from king

        // Can this pawn attack an enemy pawn (lever potential)?
        Bitboard pawnAttacks = pawn_attacks_bb(c, sq);
        if (pawnAttacks & theirPawns) {
            // This pawn can capture an enemy pawn - lever exists
            if (fileDistance <= 1) {
                score += PawnLeverOnKingFile;
            } else {
                score += PawnLeverBonus;
            }
        }

        // Also check if advancing this pawn creates a lever
        Square advanceSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
        if (advanceSq >= SQ_A1 && advanceSq <= SQ_H8) {
            if (board.piece_on(advanceSq) == NO_PIECE) {
                Bitboard advancedAttacks = pawn_attacks_bb(c, advanceSq);
                if (advancedAttacks & theirPawns) {
                    // Potential lever after advance
                    if (fileDistance <= 1) {
                        score.mg += PawnLeverOnKingFile.mg / 2;
                    } else {
                        score.mg += PawnLeverBonus.mg / 2;
                    }
                }
            }
        }
    }

    return score;
}

// ============================================================================
// Minor Piece Coordination Evaluation
// Evaluates synergy between minor pieces and with major pieces
// ============================================================================

EvalScore eval_minor_coordination(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    [[maybe_unused]] Color enemy = ~c;

    Bitboard knights = board.pieces(c, KNIGHT);
    Bitboard bishops = board.pieces(c, BISHOP);
    Bitboard queens = board.pieces(c, QUEEN);

    int knightCount = popcount(knights);
    int bishopCount = popcount(bishops);
    int queenCount = popcount(queens);

    // Queen + Knight synergy (fork potential, queen mobility + knight jumps)
    if (queenCount >= 1 && knightCount >= 1) {
        score += QueenKnightSynergy;
    }

    // Bishop + Knight coordination
    // Check if they control similar areas (complementary coverage)
    if (bishopCount >= 1 && knightCount >= 1) {
        Bitboard bishopBB = bishops;
        while (bishopBB) {
            Square bSq = pop_lsb(bishopBB);
            bool isLightBishop = ((file_of(bSq) + rank_of(bSq)) % 2) == 1;

            // Check if knight attacks squares of bishop's color
            Bitboard knightBB = knights;
            while (knightBB) {
                Square nSq = pop_lsb(knightBB);
                Bitboard knightMoves = knight_attacks_bb(nSq);

                // Count knight attacks on same color complex as bishop
                Bitboard sameColor = isLightBishop ?
                    0x55AA55AA55AA55AAULL : 0xAA55AA55AA55AA55ULL;
                int coordSquares = popcount(knightMoves & sameColor);

                if (coordSquares >= 2) {
                    score += BishopKnightCoordination;
                    break;  // Only count once per bishop
                }
            }
        }
    }

    // Knight proximity bonus - knights close together support each other
    if (knightCount >= 2) {
        Square knightSquares[2];
        int idx = 0;
        Bitboard tempBB = knights;
        while (tempBB && idx < 2) {
            knightSquares[idx++] = pop_lsb(tempBB);
        }

        if (idx >= 2) {
            int distance = std::max(
                std::abs(file_of(knightSquares[0]) - file_of(knightSquares[1])),
                std::abs(rank_of(knightSquares[0]) - rank_of(knightSquares[1]))
            );

            // Bonus for knights within 3 squares of each other
            if (distance <= 3) {
                score += KnightProximityBonus;
            }
        }
    }

    // Bishop color complex - two bishops cover all colors
    if (bishopCount >= 2) {
        bool hasLight = false;
        bool hasDark = false;
        Bitboard tempBB = bishops;
        while (tempBB) {
            Square bSq = pop_lsb(tempBB);
            if (((file_of(bSq) + rank_of(bSq)) % 2) == 1) {
                hasLight = true;
            } else {
                hasDark = true;
            }
        }

        // Opposite-colored bishops work together better
        if (hasLight && hasDark) {
            score += BishopColorComplex;
        }
    }

    return score;
}

// ============================================================================
// Piece Activity & Mobility Evaluation
// Comprehensive evaluation of piece placement, centralization, and mobility
// ============================================================================

EvalScore eval_piece_activity(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard occupied = board.pieces();
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Bitboard mobilityArea = ctx.mobilityArea[c];
    Square enemyKingSq = ctx.kingSquare[enemy];
    Square ourKingSq = ctx.kingSquare[c];

    // Count developed minor pieces (for early queen penalty)
    int developedMinors = 0;

    // =========================================================================
    // Knight Activity Evaluation
    // =========================================================================
    Bitboard knights = board.pieces(c, KNIGHT);
    while (knights) {
        Square sq = pop_lsb(knights);
        Bitboard attacks = knight_attacks_bb(sq);
        int mobility = popcount(attacks & mobilityArea);

        // Centralization bonus
        int centralIndex = get_centralization_index(sq);
        score += KnightCentralization[centralIndex];

        // Knight on the rim penalty
        File f = file_of(sq);
        Rank r = rank_of(sq);
        if (f == FILE_A || f == FILE_H || r == RANK_1 || r == RANK_8) {
            score += KnightOnRim;
        }

        // Trapped knight detection (edge knight with low mobility)
        if (mobility <= 2 && (f == FILE_A || f == FILE_H)) {
            score += TrappedKnightPenalty;
        }

        // High mobility bonus
        if (mobility >= 7) {
            score += HighMobilityBonus;
        } else if (mobility <= 2) {
            score += LowMobilityPenalty;
        }

        // King tropism
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += (7 - kingDist) * KingTropismWeight[KNIGHT];

        developedMinors++;
    }

    // =========================================================================
    // Bishop Activity Evaluation
    // =========================================================================
    Bitboard bishops = board.pieces(c, BISHOP);
    while (bishops) {
        Square sq = pop_lsb(bishops);
        Bitboard attacks = bishop_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);

        // Centralization bonus
        int centralIndex = get_centralization_index(sq);
        score += BishopCentralization[centralIndex];

        // Long diagonal bonus
        if (square_bb(sq) & (LONG_DIAGONAL_A1H8 | LONG_DIAGONAL_H1A8)) {
            // Check if bishop can see many squares on the long diagonal
            int diagMobility = popcount(attacks & (LONG_DIAGONAL_A1H8 | LONG_DIAGONAL_H1A8));
            if (diagMobility >= 3) {
                score += BishopLongDiagonal;
            }
        }

        // Fianchetto detection
        // White: bishop on g2/b2 with f2/h2 or a2/c2 pawns
        // Black: bishop on g7/b7 with f7/h7 or a7/c7 pawns
        if (c == WHITE) {
            if (sq == SQ_G2) {
                if ((ourPawns & square_bb(SQ_F2)) && (ourPawns & square_bb(SQ_H2))) {
                    score += FianchettoBonus;
                } else if (!(ourPawns & (square_bb(SQ_F2) | square_bb(SQ_G3) | square_bb(SQ_H2)))) {
                    score += FianchettoBroken;
                }
            } else if (sq == SQ_B2) {
                if ((ourPawns & square_bb(SQ_A2)) && (ourPawns & square_bb(SQ_C2))) {
                    score += FianchettoBonus;
                } else if (!(ourPawns & (square_bb(SQ_A2) | square_bb(SQ_B3) | square_bb(SQ_C2)))) {
                    score += FianchettoBroken;
                }
            }
        } else {
            if (sq == SQ_G7) {
                if ((ourPawns & square_bb(SQ_F7)) && (ourPawns & square_bb(SQ_H7))) {
                    score += FianchettoBonus;
                } else if (!(ourPawns & (square_bb(SQ_F7) | square_bb(SQ_G6) | square_bb(SQ_H7)))) {
                    score += FianchettoBroken;
                }
            } else if (sq == SQ_B7) {
                if ((ourPawns & square_bb(SQ_A7)) && (ourPawns & square_bb(SQ_C7))) {
                    score += FianchettoBonus;
                } else if (!(ourPawns & (square_bb(SQ_A7) | square_bb(SQ_B6) | square_bb(SQ_C7)))) {
                    score += FianchettoBroken;
                }
            }
        }

        // High mobility bonus
        if (mobility >= 10) {
            score += HighMobilityBonus;
        } else if (mobility <= 2) {
            score += LowMobilityPenalty;
        }

        // King tropism
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += (7 - kingDist) * KingTropismWeight[BISHOP];

        developedMinors++;
    }

    // =========================================================================
    // Rook Activity Evaluation
    // =========================================================================
    Bitboard rooks = board.pieces(c, ROOK);
    Bitboard passedPawns = 0;  // We'll detect passed pawns for rook behind passer

    // First find all passed pawns
    Bitboard tempPawns = ourPawns;
    while (tempPawns) {
        Square sq = pop_lsb(tempPawns);
        if (!(passed_pawn_mask(c, sq) & theirPawns)) {
            passedPawns |= square_bb(sq);
        }
    }

    while (rooks) {
        Square sq = pop_lsb(rooks);
        Bitboard attacks = rook_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        File f = file_of(sq);

        // File centralization
        int fileCentral = get_file_centralization(f);
        score += RookCentralization[fileCentral];

        // Rook behind passed pawn
        if (passedPawns) {
            Bitboard filePawns = file_bb(f) & passedPawns;
            if (filePawns) {
                // Check if rook is behind the passer
                Square passerSq = c == WHITE ? lsb(filePawns) : msb(filePawns);
                if ((c == WHITE && rank_of(sq) < rank_of(passerSq)) ||
                    (c == BLACK && rank_of(sq) > rank_of(passerSq))) {
                    score += RookBehindPasser;
                }
            }
        }

        // Rook on same file as enemy queen
        Bitboard enemyQueens = board.pieces(enemy, QUEEN);
        if (enemyQueens) {
            Bitboard queenFile = file_bb(f);
            if (queenFile & enemyQueens) {
                score += RookOnQueenFile;
            }
        }

        // Trapped rook by uncastled king
        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        File kingFile = file_of(ourKingSq);
        Rank kingRelRank = c == WHITE ? rank_of(ourKingSq) : Rank(RANK_8 - rank_of(ourKingSq));

        if (relRank == RANK_1 && kingRelRank == RANK_1) {
            // Rook on first rank, king also on first rank
            if ((kingFile >= FILE_E && f >= kingFile) ||  // King on kingside, rook trapped
                (kingFile <= FILE_D && f <= kingFile)) {  // King on queenside, rook trapped
                if (mobility < 4) {
                    score += TrappedRookPenalty;
                }
            }
        }

        // High mobility bonus
        if (mobility >= 12) {
            score += HighMobilityBonus;
        } else if (mobility <= 3) {
            score += LowMobilityPenalty;
        }

        // King tropism
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += (7 - kingDist) * KingTropismWeight[ROOK];
    }

    // =========================================================================
    // Queen Activity Evaluation
    // =========================================================================
    Bitboard queens = board.pieces(c, QUEEN);
    while (queens) {
        Square sq = pop_lsb(queens);
        Bitboard attacks = queen_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);

        // Centralization bonus
        int centralIndex = get_centralization_index(sq);
        score += QueenCentralization[centralIndex];

        // Early queen development penalty
        // If queen is out but minor pieces are not developed
        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        if (relRank >= RANK_3 && developedMinors < 2) {
            score += QueenEarlyDevelopment;
        }

        // High mobility bonus
        if (mobility >= 20) {
            score += HighMobilityBonus;
        } else if (mobility <= 5) {
            score += LowMobilityPenalty;
        }

        // King tropism (less weight for queen)
        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += (7 - kingDist) * KingTropismWeight[QUEEN];
    }

    return score;
}

// ============================================================================
// Advanced King Safety Evaluation
// Comprehensive evaluation of king safety including pawn shelter, storms, and attacks
// ============================================================================

EvalScore eval_king_safety_advanced(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;
    Square kingSq = ctx.kingSquare[c];
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Bitboard occupied = board.pieces();
    File kingFile = file_of(kingSq);
    Rank kingRank = rank_of(kingSq);
    Rank kingRelRank = c == WHITE ? kingRank : Rank(RANK_8 - kingRank);

    int attackUnits = 0;
    int attackerCount = ctx.kingAttackersCount[enemy];

    // =========================================================================
    // Pawn Shield Evaluation (Detailed)
    // =========================================================================
    if (kingRelRank <= RANK_2) {  // King is on back ranks
        int shieldScore = 0;
        int missingPawns = 0;

        for (int df = -2; df <= 2; df++) {
            File f = File(kingFile + df);
            if (f < FILE_A || f > FILE_H) continue;

            int fileIndex = df + 2;  // Map to 0-4 index
            Bitboard filePawns = file_bb(f) & ourPawns;

            if (!filePawns) {
                // No pawn on this file
                shieldScore += PawnShieldQuality[fileIndex][0];
                missingPawns++;
            } else {
                // Find the most advanced pawn (closest to king)
                Square pawnSq = c == WHITE ? lsb(filePawns) : msb(filePawns);
                Rank pawnRank = rank_of(pawnSq);
                Rank pawnRelRank = c == WHITE ? pawnRank : Rank(RANK_8 - pawnRank);

                // Calculate distance from ideal position
                int idealRank = c == WHITE ? 1 : 6;  // Rank 2 for white, rank 7 for black
                int distance = std::abs(pawnRank - idealRank);

                int distIndex = std::min(distance, 3);
                shieldScore += PawnShieldQuality[fileIndex][distIndex + 1];
            }
        }

        score.mg += shieldScore;

        // Shelter weakness based on missing pawns
        if (missingPawns > 0 && missingPawns <= 3) {
            score += ShelterWeakness[missingPawns];
        }
    }

    // =========================================================================
    // Pawn Storm Evaluation (Enemy pawns advancing toward our king)
    // =========================================================================
    if (kingRelRank <= RANK_2) {
        int stormScore = 0;

        for (int df = -2; df <= 2; df++) {
            File f = File(kingFile + df);
            if (f < FILE_A || f > FILE_H) continue;

            int fileIndex = df + 2;
            Bitboard filePawns = file_bb(f) & theirPawns;

            if (filePawns) {
                // Find the most advanced enemy pawn
                Square pawnSq = c == WHITE ? msb(filePawns) : lsb(filePawns);
                Rank pawnRelRank = c == WHITE ? rank_of(pawnSq) : Rank(RANK_8 - rank_of(pawnSq));

                // Map rank to danger index
                int dangerIndex;
                if (pawnRelRank <= RANK_3) dangerIndex = 1;       // Far
                else if (pawnRelRank <= RANK_5) dangerIndex = 2;  // Mid
                else dangerIndex = 3;                              // Close

                stormScore += PawnStormDanger[fileIndex][dangerIndex];
            }
        }

        score.mg -= stormScore;
    }

    // =========================================================================
    // Attack Unit Calculation with Refined Weights
    // =========================================================================

    // Use cached attack information from context
    attackUnits += ctx.kingAttackersWeight[enemy];

    // Add inner/outer ring attack contributions
    attackUnits += ctx.innerRingAttacks[enemy] * 2;
    attackUnits += ctx.outerRingAttacks[enemy];

    // =========================================================================
    // Safe Check Detection
    // =========================================================================
    Bitboard unsafeFor = ctx.attackedBy[c][ALL_PIECES];
    Bitboard safeSquares = ~unsafeFor & ~board.pieces(enemy);

    // Knight checks
    Bitboard knightChecks = knight_attacks_bb(kingSq) & safeSquares;
    if (ctx.attackedBy[enemy][KNIGHT] & knightChecks) {
        attackUnits += SafeCheckBonus[KNIGHT];
    }

    // Bishop checks
    Bitboard bishopChecks = bishop_attacks_bb(kingSq, occupied) & safeSquares;
    if (ctx.attackedBy[enemy][BISHOP] & bishopChecks) {
        attackUnits += SafeCheckBonus[BISHOP];
    }

    // Rook checks
    Bitboard rookChecks = rook_attacks_bb(kingSq, occupied) & safeSquares;
    if (ctx.attackedBy[enemy][ROOK] & rookChecks) {
        attackUnits += SafeCheckBonus[ROOK];
    }

    // Queen checks
    Bitboard queenChecks = queen_attacks_bb(kingSq, occupied) & safeSquares;
    if (ctx.attackedBy[enemy][QUEEN] & queenChecks) {
        attackUnits += SafeCheckBonus[QUEEN];
    }

    // Contact checks (pieces directly attacking adjacent squares)
    Bitboard kingZone = king_attacks_bb(kingSq);
    Bitboard contactAttacks = kingZone & ctx.attackedBy[enemy][ALL_PIECES] & safeSquares;
    if (contactAttacks) {
        attackUnits += ContactCheckBonus * popcount(contactAttacks);
    }

    // =========================================================================
    // Virtual Mobility (potential slider attacks through king position)
    // =========================================================================
    Bitboard virtualBishop = bishop_attacks_bb(kingSq, occupied ^ board.pieces(c, QUEEN));
    Bitboard virtualRook = rook_attacks_bb(kingSq, occupied ^ board.pieces(c, QUEEN));

    // Count enemy sliders that could attack if queen moves
    attackUnits += popcount(virtualBishop & board.pieces(enemy, BISHOP, QUEEN)) * VirtualMobilityWeight;
    attackUnits += popcount(virtualRook & board.pieces(enemy, ROOK, QUEEN)) * VirtualMobilityWeight;

    // =========================================================================
    // Open/Semi-Open Files Near King
    // =========================================================================
    for (int df = -1; df <= 1; df++) {
        File f = File(kingFile + df);
        if (f < FILE_A || f > FILE_H) continue;

        Bitboard fileMask = file_bb(f);
        bool hasOurPawn = (fileMask & ourPawns) != 0;
        bool hasTheirPawn = (fileMask & theirPawns) != 0;

        if (!hasOurPawn) {
            if (!hasTheirPawn) {
                // Open file
                score.mg -= KingExposedOnFile;
                attackUnits += 5;
            } else {
                // Semi-open (no our pawn, but enemy has pawn)
                score.mg -= KingExposedOnFile / 2;
                attackUnits += 2;
            }
        }
    }

    // =========================================================================
    // Weak Squares in King Zone
    // =========================================================================
    Bitboard ourDefended = ctx.attackedBy[c][ALL_PIECES];
    Bitboard enemyAttacked = ctx.attackedBy[enemy][ALL_PIECES];

    // Inner ring weak squares
    Bitboard innerWeak = ctx.innerKingRing[c] & ~ourDefended & enemyAttacked;
    attackUnits += popcount(innerWeak) * 3;

    // Outer ring weak squares
    Bitboard outerWeak = ctx.outerKingRing[c] & ~ourDefended & enemyAttacked;
    attackUnits += popcount(outerWeak);

    // =========================================================================
    // Final Safety Calculation
    // =========================================================================
    if (attackerCount >= MinAttackersForDanger) {
        int penalty = KingSafetyTable[std::min(attackUnits, 99)];
        penalty = penalty * Tuning::KingSafetyWeight / 100;
        penalty = std::min(penalty, MaxKingSafetyPenalty);
        score.mg -= penalty;
    }

    return score;
}

// ============================================================================
// Main Evaluation Function
// ============================================================================

int evaluate(const Board& board, int alpha, int beta) {
    PROFILE_SCOPE("Eval::evaluate");
    EvalScore score;
    int phase = 0;
    phase += popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase += popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase += popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase += popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::min(phase, TotalPhase);

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

    // =========================================================================
    // LAZY EVALUATION - Quick check for clearly won/lost positions
    // Only skip expensive calculations if position is VERY far from window
    // =========================================================================
    {
        int mg = score.mg;
        int eg = score.eg;
        int lazyScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;
        lazyScore = board.side_to_move() == WHITE ? lazyScore : -lazyScore;

        // Use larger margin (600cp) since we want accurate eval for tactics
        constexpr int LAZY_MARGIN = 600;
        if (lazyScore >= beta + LAZY_MARGIN || lazyScore <= alpha - LAZY_MARGIN) {
            return lazyScore;
        }
    }

    // =========================================================================
    // FULL EVALUATION with EvalContext
    // Initialize attack maps ONCE and reuse across all evaluation components
    // This is more efficient than computing attacks separately in each function
    // =========================================================================
    EvalContext ctx;
    init_eval_context(ctx, board);

    // Piece mobility and activity evaluation (uses cached attack maps)
    score += eval_pieces_with_context(board, WHITE, ctx);
    score -= eval_pieces_with_context(board, BLACK, ctx);

    // King safety evaluation (uses cached attack maps and king ring info)
    score += eval_king_safety_with_context(board, WHITE, ctx);
    score -= eval_king_safety_with_context(board, BLACK, ctx);

    // =========================================================================
    // THREAT DETECTION - Critical for tactical accuracy!
    // Evaluates hanging pieces, attacks on valuable pieces, etc.
    // =========================================================================
    score += eval_threats_with_context(board, WHITE, ctx);
    score -= eval_threats_with_context(board, BLACK, ctx);

    // Space evaluation (control of central squares)
    score += eval_space(board, WHITE);
    score -= eval_space(board, BLACK);

    // Material imbalance (piece combination bonuses/penalties)
    score += eval_material_imbalance(board, WHITE);
    score -= eval_material_imbalance(board, BLACK);

    // Tapered evaluation
    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    // Return from side to move's perspective with tempo bonus
    constexpr int TEMPO = 12;
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
    int phase = 0;
    phase += popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase += popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase += popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase += popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::min(phase, TotalPhase);

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

    // Material imbalance (piece combination bonuses/penalties)
    score += eval_material_imbalance(board, WHITE);
    score -= eval_material_imbalance(board, BLACK);

    // Pawn lever detection (pawn storm threats)
    score += eval_pawn_levers(board, WHITE, ctx);
    score -= eval_pawn_levers(board, BLACK, ctx);

    // Minor piece coordination (synergy bonuses)
    score += eval_minor_coordination(board, WHITE, ctx);
    score -= eval_minor_coordination(board, BLACK, ctx);

    // Tapered evaluation
    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    // [FIX] Tempo bonus consistent at 12cp
    constexpr int TEMPO = 12;

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

