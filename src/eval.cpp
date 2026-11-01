#include "eval.hpp"
#include "tuning.hpp"
#include "profiler.hpp"

namespace Eval {

thread_local PawnTable pawnTable;

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

    Bitboard neighbors = adjacent_files_bb(f) & ourPawns;
    if (!neighbors) return false;

    Bitboard behindMask = c == WHITE ?
        (0xFFFFFFFFFFFFFFFFULL >> (64 - r * 8)) :
        (0xFFFFFFFFFFFFFFFFULL << ((r + 1) * 8));

    if (neighbors & ~behindMask) return false;

    Square stopSq = pawn_push(c, s);
    Bitboard stopAttacks = c == WHITE ?
        ((square_bb(stopSq) >> 7) & ~file_bb(FILE_A)) | ((square_bb(stopSq) >> 9) & ~file_bb(FILE_H)) :
        ((square_bb(stopSq) << 7) & ~file_bb(FILE_H)) | ((square_bb(stopSq) << 9) & ~file_bb(FILE_A));

    return (stopAttacks & theirPawns) != 0;
}

int get_centralization_index(Square sq) {
    File f = file_of(sq);
    Rank r = rank_of(sq);

    int fileDistFromCenter = std::min(std::abs(f - FILE_D), std::abs(f - FILE_E));
    int rankDistFromCenter = std::min(std::abs(r - RANK_4), std::abs(r - RANK_5));
    int maxDist = std::max(fileDistFromCenter, rankDistFromCenter);

    if (maxDist == 0) return 3;
    if (maxDist == 1) return 2;
    if (maxDist == 2) return 1;
    return 0;
}

int get_file_centralization(File f) {
    if (f == FILE_D || f == FILE_E) return 3;
    if (f == FILE_C || f == FILE_F) return 2;
    if (f == FILE_B || f == FILE_G) return 1;
    return 0;
}

constexpr Bitboard LONG_DIAGONAL_A1H8 = 0x8040201008040201ULL;
constexpr Bitboard LONG_DIAGONAL_H1A8 = 0x0102040810204080ULL;

static Bitboard compute_outer_ring(Square kingSq) {
    static Bitboard table[SQUARE_NB] = {};
    static bool initialized = false;
    if (!initialized) {
        for (int s = SQ_A1; s <= SQ_H8; ++s) {
            Bitboard inner = king_attacks_bb(Square(s)) | square_bb(Square(s));
            Bitboard outer = 0ULL;
            Bitboard temp = inner;
            while (temp) {
                Square sq = pop_lsb(temp);
                outer |= king_attacks_bb(sq);
            }
            table[s] = outer & ~inner;
        }
        initialized = true;
    }
    return table[kingSq];
}

void init_eval_context(EvalContext& ctx, const Board& board) {
    ctx.clear();
    Bitboard occupied = board.pieces();

    for (Color c : {WHITE, BLACK}) {
        Color enemy = ~c;
        ctx.kingSquare[c] = board.king_square(c);

        Bitboard ourPawns = board.pieces(c, PAWN);
        ctx.attackedBy[c][PAWN] = pawn_attacks_bb(c, ourPawns);
        ctx.attackedBy[c][ALL_PIECES] = ctx.attackedBy[c][PAWN];

        Square kingSq = ctx.kingSquare[c];
        ctx.innerKingRing[c] = king_attacks_bb(kingSq) | square_bb(kingSq);
        ctx.outerKingRing[c] = compute_outer_ring(kingSq);

        ctx.kingRing[c] = ctx.innerKingRing[c];
        if (c == WHITE) {
            ctx.kingRing[c] |= (ctx.kingRing[c] << 8) & ~RANK_8_BB;
        } else {
            ctx.kingRing[c] |= (ctx.kingRing[c] >> 8) & ~RANK_1_BB;
        }

        ctx.attackedBy[c][KING] = king_attacks_bb(kingSq);
        ctx.attackedBy2[c] = ctx.attackedBy[c][PAWN] & ctx.attackedBy[c][KING];
        ctx.attackedBy[c][ALL_PIECES] |= ctx.attackedBy[c][KING];

        Bitboard enemyPawnAttacks = pawn_attacks_bb(enemy, board.pieces(enemy, PAWN));
        ctx.mobilityArea[c] = ~(board.pieces(c) | enemyPawnAttacks);
    }

    for (Color c : {WHITE, BLACK}) {
        Color enemy = ~c;

        Bitboard knights = board.pieces(c, KNIGHT);
        Bitboard knightAttacks = EMPTY_BB;
        while (knights) {
            Square sq = pop_lsb(knights);
            Bitboard attacks = knight_attacks_bb(sq);
            knightAttacks |= attacks;
            ctx.attackedBy2[c] |= ctx.attackedBy[c][ALL_PIECES] & attacks;
            ctx.attackedBy[c][ALL_PIECES] |= attacks;

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

static bool is_outpost_hole(Color c, Square sq, Bitboard ourPawns) {
    File f = file_of(sq);
    Rank r = rank_of(sq);

    Bitboard adjacentPawns = adjacent_files_bb(f) & ourPawns;
    if (!adjacentPawns) return true;

    if (c == WHITE) {
        Bitboard behindMask = 0ULL;
        for (int rr = RANK_1; rr < r; ++rr) {
            behindMask |= rank_bb_eval(Rank(rr));
        }
        if (adjacentPawns & behindMask) return false;
    } else {
        Bitboard behindMask = 0ULL;
        for (int rr = r + 1; rr <= RANK_8; ++rr) {
            behindMask |= rank_bb_eval(Rank(rr));
        }
        if (adjacentPawns & behindMask) return false;
    }

    return true;
}

EvalScore eval_pawn_structure(const Board& board, Color c, Bitboard& outPassedPawns, Bitboard& outHangingPawns) {
    EvalScore score;
    Color enemy = ~c;
    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    int islands = count_pawn_islands(ourPawns);
    if (islands > 1) {
        score.mg += PawnIslandPenalty.mg * (islands - 1);
        score.eg += PawnIslandPenalty.eg * (islands - 1);
    }

    constexpr Bitboard WhiteCentralSquares = (1ULL << SQ_D4) | (1ULL << SQ_E4);
    constexpr Bitboard BlackCentralSquares = (1ULL << SQ_D5) | (1ULL << SQ_E5);
    Bitboard centralSquares = c == WHITE ? WhiteCentralSquares : BlackCentralSquares;
    int centralPawns = popcount(ourPawns & centralSquares);
    score += CentralPawnBonus * centralPawns;


    Bitboard passedPawns = 0;
    Bitboard hangingPawns = 0;
    Bitboard tempBb = ourPawns;
    while (tempBb) {
        Square sq = pop_lsb(tempBb);
        if (!(passed_pawn_mask(c, sq) & theirPawns)) {
            passedPawns |= square_bb(sq);
        }
    }

    Bitboard pawnDefenders = pawn_attacks_bb(c, ourPawns);

    Bitboard bb = ourPawns;
    while (bb) {
        Square sq = pop_lsb(bb);
        File f = file_of(sq);
        Rank r = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        bool isIsolated = !(adjacent_files_bb(f) & ourPawns);

        bool isPassed = (passedPawns & square_bb(sq)) != 0;
        if (isPassed) {
            score += PassedPawnBonus[r];

            Bitboard adjacentPassers = adjacent_files_bb(f) & passedPawns;
            if (adjacentPassers) {
                Bitboard supportRange = rank_bb_eval(rank_of(sq));
                if (rank_of(sq) > RANK_1) supportRange |= rank_bb_eval(Rank(rank_of(sq) - 1));
                if (rank_of(sq) < RANK_8) supportRange |= rank_bb_eval(Rank(rank_of(sq) + 1));

                if (adjacentPassers & supportRange) {
                    score += ConnectedPassedBonus[r];
                }
            }

            if (square_bb(sq) & pawnDefenders) {
                score += ProtectedPassedBonus[r];
                score -= PassedPawnBonus[r];
            }
        }
        else {
            Bitboard stoppers = passed_pawn_mask(c, sq) & theirPawns;
            int stopperCount = popcount(stoppers);

            if (stopperCount == 1) {
                Bitboard helperMask = adjacent_files_bb(f) & ourPawns;
                bool hasHelper = false;

                if (c == WHITE) {
                    Bitboard behindOrEqual = ~0ULL >> (8 * (8 - rank_of(sq)));
                    if (helperMask & behindOrEqual) hasHelper = true;
                } else {
                    Bitboard behindOrEqual = ~0ULL << (8 * (rank_of(sq) + 1));
                    if (helperMask & behindOrEqual) hasHelper = true;
                }

                Bitboard pawnAttacks = pawn_attacks_bb(c, sq);
                if (pawnAttacks & stoppers) {
                    hasHelper = true;
                }

                if (hasHelper) {
                    score += CandidatePassedBonus[r];
                }
            }
        }

        if (isIsolated) {
            score += IsolatedPawnPenalty;
        }

        if (popcount(file_bb(f) & ourPawns) > 1) {
            score += DoubledPawnPenalty;
        }

        if (!isIsolated && !isPassed && is_backward_pawn(c, sq, ourPawns, theirPawns)) {
            score += BackwardPawnPenalty;

            Bitboard fileMask = file_bb(f);
            if (!(fileMask & theirPawns)) {
                score += BackwardOnHalfOpen;
            }
        }

        Bitboard adjacentPawns = adjacent_files_bb(f) & ourPawns;
        if (adjacentPawns) {
            if (adjacentPawns & rank_bb_eval(rank_of(sq))) {
                score += PhalanxBonus;
                score += PawnDuoBonus;
            } else {
                score += ConnectedPawnBonus;
            }

            if (square_bb(sq) & pawnDefenders) {
                score += PawnChainBonus;

                Bitboard thisDefends = pawn_attacks_bb(c, sq);
                if (thisDefends & ourPawns) {
                    score += PawnChainBaseBonus;
                }
            }
        }

        if (!isIsolated && !isPassed) {
            Bitboard leftFile = (f > FILE_A) ? file_bb(File(f - 1)) : 0;
            Bitboard rightFile = (f < FILE_H) ? file_bb(File(f + 1)) : 0;

            bool hasLeftNeighbor = (leftFile & ourPawns) != 0;
            bool hasRightNeighbor = (rightFile & ourPawns) != 0;

            if (hasLeftNeighbor != hasRightNeighbor) {
                File neighborFile = hasLeftNeighbor ? File(f - 1) : File(f + 1);

                bool ourFileSemiOpen = !(file_bb(f) & theirPawns);
                bool neighborFileSemiOpen = !(file_bb(neighborFile) & theirPawns);

                if (ourFileSemiOpen && neighborFileSemiOpen) {
                    File outerFile = hasLeftNeighbor ? File(f - 2) : File(f + 2);
                    bool hasWiderSupport = false;
                    if (outerFile >= FILE_A && outerFile <= FILE_H) {
                        hasWiderSupport = (file_bb(outerFile) & ourPawns) != 0;
                    }

                    if (!hasWiderSupport) {
                        score += HangingPawnPenalty;
                        hangingPawns |= square_bb(sq);
                    }
                }
            }
        }
    }

    constexpr Bitboard OutpostZoneWhite = 0x00FFFFFF00000000ULL;
    constexpr Bitboard OutpostZoneBlack = 0x00000000FFFFFF00ULL;
    Bitboard outpostZone = c == WHITE ? OutpostZoneWhite : OutpostZoneBlack;

    for (File f = FILE_C; f <= FILE_F; ++f) {
        Bitboard fileZone = file_bb(f) & outpostZone;
        Bitboard temp = fileZone;
        while (temp) {
            Square sq = pop_lsb(temp);
            if (is_outpost_hole(c, sq, ourPawns)) {
                score += OutpostHolePenalty;
            }
        }
    }

    outPassedPawns = passedPawns;
    outHangingPawns = hangingPawns;
    return score;
}

EvalScore eval_passed_pawns_dynamic(const Board& board, Color c, Bitboard passedPawns, Bitboard hangingPawns) {
    EvalScore score;
    if (!passedPawns && !hangingPawns) return score;

    Color enemy = ~c;
    Bitboard ourRooks = board.pieces(c, ROOK);
    Square ourKingSq = board.king_square(c);
    Square enemyKingSq = board.king_square(enemy);
    Bitboard enemyPiecesNoKing = board.pieces(enemy) ^ board.pieces(enemy, KING);

    while (passedPawns) {
        Square sq = pop_lsb(passedPawns);
        File f = file_of(sq);

        int ourKingDist = std::max(std::abs(file_of(ourKingSq) - f),
                                   std::abs(rank_of(ourKingSq) - rank_of(sq)));
        int enemyKingDist = std::max(std::abs(file_of(enemyKingSq) - f),
                                     std::abs(rank_of(enemyKingSq) - rank_of(sq)));
        score.eg += (enemyKingDist - ourKingDist) * 5;

        Bitboard behindPawn = c == WHITE ?
            (file_bb(f) & (square_bb(sq) - 1)) :
            (file_bb(f) & ~(square_bb(sq) | (square_bb(sq) - 1)));
        if (behindPawn & ourRooks) {
            score.mg += 15;
            score.eg += 25;
        }

        Square stopSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
        Rank r = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        Bitboard pawnPath = c == WHITE ?
            (file_bb(f) & ~(square_bb(sq) | (square_bb(sq) - 1))) :
            (file_bb(f) & (square_bb(sq) - 1));

        if (stopSq >= SQ_A1 && stopSq <= SQ_H8) {
            Piece blocker = board.piece_on(stopSq);
            if (blocker != NO_PIECE && color_of(blocker) == enemy) {
                score.mg -= (r >= RANK_6) ? 20 : 10;
                score.eg -= (r >= RANK_6) ? 55 : 25;
            } else if (blocker != NO_PIECE && color_of(blocker) == c && r == RANK_7) {
                score -= S(30, 70);
            } else if (blocker == NO_PIECE && r >= RANK_5) {
                if (!board.is_attacked_by(enemy, stopSq)) {
                    if (r == RANK_5)      score += S(15, 35);
                    else if (r == RANK_6) score += S(45, 110);
                    else if (r == RANK_7) {
                        score += S(120, 260);
                        if ((behindPawn & ourRooks) || board.is_attacked_by(c, sq)) {
                            score += S(60, 140);
                        }
                    }
                } else if (board.is_attacked_by(c, stopSq)) {
                    if (r == RANK_5)      score += S(10, 25);
                    else if (r == RANK_6) score += S(20, 50);
                    else if (r == RANK_7) score += S(55, 130);
                }
            }
            if (r >= RANK_5 && board.pieces(c, BISHOP)) {
                Bitboard bbb = board.pieces(c, BISHOP);
                while (bbb) {
                    Square bSq = pop_lsb(bbb);
                    if (bishop_attacks_bb(bSq, board.pieces(c)) & square_bb(stopSq)) {
                        score += S(10, 28);
                    }
                }
            }
        }

        Rank promoRank = c == WHITE ? RANK_8 : RANK_1;
        int stepsToPromo = std::abs(int(promoRank) - int(rank_of(sq)));
        if (stepsToPromo == 6) {
            stepsToPromo = 5;
        }
        int effectiveSquares = stepsToPromo + (board.side_to_move() != c ? 1 : 0);
        int kingToPromo = std::max(
            std::abs(int(file_of(enemyKingSq)) - int(f)),
            std::abs(int(rank_of(enemyKingSq)) - int(promoRank))
        );

        if (kingToPromo > effectiveSquares) {
            Bitboard enemyNonPawns = enemyPiecesNoKing & ~board.pieces(enemy, PAWN);
            if (enemyNonPawns == 0 && !(pawnPath & board.pieces())) {
                score.eg += 550 - stepsToPromo * 25;
            } else if (!(pawnPath & enemyPiecesNoKing)) {
                score.eg += RuleOfSquareBonus;
            }
        }
    }

    while (hangingPawns) {
        Square sq = pop_lsb(hangingPawns);
        Square advSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
        if (advSq >= SQ_A1 && advSq <= SQ_H8 && board.piece_on(advSq) == NO_PIECE) {
            score += HangingPawnWithThreat;
        }
    }

    return score;
}

EvalScore eval_space(const Board& board, Color c) {
    EvalScore score;
    Color enemy = ~c;

    constexpr Bitboard WhiteSpaceArea = 0x00003C3C3C000000ULL;
    constexpr Bitboard BlackSpaceArea = 0x000000003C3C3C00ULL;

    Bitboard spaceArea = (c == WHITE) ? WhiteSpaceArea : BlackSpaceArea;

    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);

    Bitboard behindPawns;
    if (c == WHITE) {
        behindPawns = ourPawns >> 8;
        behindPawns |= behindPawns >> 8;
        behindPawns |= behindPawns >> 16;
    } else {
        behindPawns = ourPawns << 8;
        behindPawns |= behindPawns << 8;
        behindPawns |= behindPawns << 16;
    }

    Bitboard enemyPawnAttacks = pawn_attacks_bb(enemy, theirPawns);
    Bitboard safe = spaceArea & (behindPawns | ~enemyPawnAttacks);
    int spaceCount = popcount(safe);

    int pieceCount = popcount(board.pieces(c)) - 1;
    int spaceBonus = (spaceCount * spaceCount * pieceCount) / 128;

    score.mg = spaceBonus;
    score.eg = spaceBonus / 2;

    return score;
}

EvalScore eval_threats_with_context(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;

    Bitboard attacked = ctx.attackedBy[c][ALL_PIECES];
    Bitboard enemyPieces = board.pieces(enemy);
    Bitboard defended = ctx.attackedBy[enemy][ALL_PIECES];
    Bitboard hanging = enemyPieces & attacked & ~defended;
    score.mg += popcount(hanging) * 25;
    score.eg += popcount(hanging) * 20;

    Bitboard enemyNonPawns = enemyPieces ^ board.pieces(enemy, PAWN, KING);
    Bitboard hangingNonPawns = enemyNonPawns & attacked & ~defended;
    score.mg += popcount(hangingNonPawns) * 40;
    score.eg += popcount(hangingNonPawns) * 35;

    Bitboard attackedByPawn = ctx.attackedBy[c][PAWN];
    Bitboard attackedByMinor = ctx.attackedBy[c][KNIGHT] | ctx.attackedBy[c][BISHOP];
    Bitboard minorsAttacked = (board.pieces(enemy, KNIGHT) | board.pieces(enemy, BISHOP)) & attackedByPawn;
    score.mg += popcount(minorsAttacked) * 40;
    score.eg += popcount(minorsAttacked) * 30;
    Bitboard rooksAttacked = board.pieces(enemy, ROOK) & (attackedByPawn | attackedByMinor);
    score.mg += popcount(rooksAttacked) * 45;
    score.eg += popcount(rooksAttacked) * 35;
    Bitboard queensAttacked = board.pieces(enemy, QUEEN) &
        (attackedByPawn | attackedByMinor | ctx.attackedBy[c][ROOK]);
    score.mg += popcount(queensAttacked) * 55;
    score.eg += popcount(queensAttacked) * 45;

    Bitboard contested = ctx.attackedBy[c][ALL_PIECES] & ctx.attackedBy[enemy][ALL_PIECES];
    Bitboard ourDouble = contested & ctx.attackedBy2[c];
    Bitboard theirSingle = contested & ~ctx.attackedBy2[enemy];
    Bitboard advantage = ourDouble & theirSingle;
    score.mg += popcount(advantage) * 8;
    score.eg += popcount(advantage) * 5;

    Bitboard overloadedNonPawns = enemyNonPawns & advantage;
    Bitboard pieceThreats = hangingNonPawns | overloadedNonPawns | minorsAttacked | rooksAttacked | queensAttacked;
    int threatCount = popcount(pieceThreats);
    if (threatCount >= 2) {
        score.mg += 110 * (threatCount - 1);
        score.eg += 95 * (threatCount - 1);
        if (popcount(rooksAttacked | queensAttacked) >= 2) {
            score.mg += 80;
            score.eg += 70;
        }
    }

    Bitboard weakEnemyPawns = board.pieces(enemy, PAWN) & ~ctx.attackedBy[enemy][PAWN];
    Bitboard bishopAttackedWeakPawns = weakEnemyPawns & ctx.attackedBy[c][BISHOP];
    score.mg += popcount(bishopAttackedWeakPawns) * 14;
    score.eg += popcount(bishopAttackedWeakPawns) * 26;
    if (bishopAttackedWeakPawns & ctx.innerKingRing[enemy]) {
        score.mg += 25;
        score.eg += 15;
    }

    Bitboard ourPieces = board.pieces(c) & ~board.pieces(c, PAWN) & ~square_bb(board.king_square(c));
    Bitboard ourDefended = ctx.attackedBy[c][ALL_PIECES];

    Bitboard connectedPieces = ourPieces & ourDefended;
    int connectedCount = popcount(connectedPieces);

    if (connectedCount >= 3) {
        score.mg += (connectedCount - 2) * 8;
        score.eg += (connectedCount - 2) * 5;
    }

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

    Bitboard bb = board.pieces(c, KNIGHT);
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = knight_attacks_bb(sq);
        int mobility = popcount(attacks & mobilityArea);
        score += KnightMobility[std::min(mobility, 8)];

        if (ctx.attackedBy[c][PAWN] & square_bb(sq)) {
            Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
            if (relRank >= RANK_4 && relRank <= RANK_6) {
                score += KnightOutpostBonus;
            }
        }

        int kingDist = std::max(std::abs(file_of(sq) - file_of(enemyKingSq)),
                                std::abs(rank_of(sq) - rank_of(enemyKingSq)));
        score.mg += std::max(0, (5 - kingDist) * 3);
    }

    bb = board.pieces(c, BISHOP);
    int bishopCount = 0;
    while (bb) {
        bishopCount++;
        Square sq = pop_lsb(bb);
        Bitboard attacks = bishop_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += BishopMobility[std::min(mobility, 13)];

        bool isLight = ((file_of(sq) + rank_of(sq)) % 2) == 1;
        Bitboard sameColor = isLight ? 0x55AA55AA55AA55AAULL : 0xAA55AA55AA55AA55ULL;
        int blocked = popcount(ourPawns & sameColor);
        if (blocked >= 4) {
            score.mg -= (blocked - 3) * 8;
            score.eg -= (blocked - 3) * 5;
        }

        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        if (relRank == RANK_7) {
            File f = file_of(sq);
            if (f == FILE_A) {
                Square block = c == WHITE ? SQ_B6 : SQ_B3;
                if (theirPawns & square_bb(block)) { score.mg -= 100; score.eg -= 80; }
            } else if (f == FILE_H) {
                Square block = c == WHITE ? SQ_G6 : SQ_G3;
                if (theirPawns & square_bb(block)) { score.mg -= 100; score.eg -= 80; }
            }
        }
    }
    if (bishopCount >= 2) score += BishopPairBonus;

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

        Bitboard filePawns = file_bb(f);
        if (!(filePawns & ourPawns)) {
            if (!(filePawns & theirPawns)) score += RookOpenFileBonus;
            else score += RookSemiOpenFileBonus;
        }

        Rank relRank = c == WHITE ? rank_of(sq) : Rank(RANK_8 - rank_of(sq));
        if (relRank == RANK_7) score += RookOnSeventhBonus;
    }

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

    bb = board.pieces(c, QUEEN);
    Bitboard unsafeForQueen = ctx.attackedBy[enemy][PAWN] | ctx.attackedBy[enemy][KNIGHT] |
                              ctx.attackedBy[enemy][BISHOP] | ctx.attackedBy[enemy][ROOK];
    Bitboard defendedEnemy = board.pieces(enemy) & ctx.attackedBy[enemy][ALL_PIECES];
    while (bb) {
        Square sq = pop_lsb(bb);
        Bitboard attacks = queen_attacks_bb(sq, occupied);
        int mobility = popcount(attacks & mobilityArea);
        score += QueenMobility[std::min(mobility, 27)];

        Bitboard safeMoves = attacks & ~board.pieces(c) & ~unsafeForQueen & ~defendedEnemy;
        int safeCount = popcount(safeMoves);
        if (safeCount == 0) {
            score += S(-180, -150);
        } else if (safeCount == 1) {
            score += S(-95, -80);
        } else if (safeCount == 2 && (unsafeForQueen & square_bb(sq))) {
            score += S(-45, -35);
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

    Bitboard virtualQ = queen_attacks_bb(kingSq, board.pieces());
    Bitboard enemySliders = board.pieces(enemy, BISHOP, QUEEN) | board.pieces(enemy, ROOK);
    attackUnits += popcount(virtualQ & enemySliders);
    Bitboard unsafeFor = ctx.attackedBy[c][ALL_PIECES];
    Bitboard safeSquares = ~unsafeFor & ~board.pieces(enemy);

    if (ctx.attackedBy[enemy][KNIGHT] & (knight_attacks_bb(kingSq) & safeSquares)) attackUnits += SafeCheckBonus[KNIGHT];
    if (ctx.attackedBy[enemy][BISHOP] & (bishop_attacks_bb(kingSq, board.pieces()) & safeSquares)) attackUnits += SafeCheckBonus[BISHOP];
    if (ctx.attackedBy[enemy][ROOK] & (rook_attacks_bb(kingSq, board.pieces()) & safeSquares)) attackUnits += SafeCheckBonus[ROOK];
    if (ctx.attackedBy[enemy][QUEEN] & (king_attacks_bb(kingSq) & safeSquares)) attackUnits += SafeCheckBonus[QUEEN];

    if (attackCount >= 2) {
        int penalty = KingSafetyTable[std::min(attackUnits, 99)];
        penalty = penalty * Tuning::KingSafetyWeight / 100;
        score.mg -= penalty;
    }

    Bitboard ourDefended = ctx.attackedBy[c][ALL_PIECES];
    Bitboard innerWeak = ctx.innerKingRing[c] & ~ourDefended & ~board.pieces();
    Bitboard outerWeak = ctx.outerKingRing[c] & ~ourDefended & ~board.pieces();

    int innerWeakCount = popcount(innerWeak & ctx.attackedBy[enemy][ALL_PIECES]);
    int outerWeakCount = popcount(outerWeak & ctx.attackedBy[enemy][ALL_PIECES]);

    score.mg -= innerWeakCount * InnerRingWeakSquarePenalty;
    score.mg -= outerWeakCount * OuterRingWeakSquarePenalty;

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

    File ourKingFile = file_of(kingSq);
    for (int df = -1; df <= 1; df++) {
        File f = File(ourKingFile + df);
        if (f < FILE_A || f > FILE_H) continue;
        Bitboard fileMask = file_bb(f);
        if (!(fileMask & ourPawns)) {
            if (!(fileMask & theirPawns)) { score.mg -= KingOpenFilePenalty.mg; score.eg -= KingOpenFilePenalty.eg; }
            else { score.mg -= KingSemiOpenFilePenalty.mg; score.eg -= KingSemiOpenFilePenalty.eg; }
        }
    }

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

    if (ourRooks >= 2) {
        score += RookPairBonus;
    }

    if (ourKnights >= 2) {
        score += KnightPairPenalty;
    }

    if (ourBishops >= 1 && ourKnights >= 1) {
        score += BishopKnightCombo;
    }

    if (ourRooks >= 1 && ourQueens == 0 && theirQueens == 0) {
        score += RooksWithoutQueens;
    }

    if (ourQueens >= 1 && ourRooks == 0) {
        score += QueenWithoutRooks;
    }

    if (ourQueens >= 1 && theirQueens == 0 && (theirRooks + theirKnights + theirBishops >= 2)) {
        score += S(20, 20);
    }

    int ourMinors = ourKnights + ourBishops;
    int theirMinors = theirKnights + theirBishops;
    int ourPawnCount = popcount(board.pieces(c, PAWN));
    int theirPawnCount = popcount(board.pieces(enemy, PAWN));
    if (ourQueens == 0 && theirQueens == 0 && ourRooks == 2 && theirRooks == 1 &&
        ourMinors == 1 && theirBishops == 1 && theirKnights == 1 &&
        ourPawnCount < theirPawnCount) {
        score += S(70, 70);
    }
    if (ourQueens == theirQueens && ourRooks == 2 && theirRooks == 2 &&
        ourBishops == 1 && ourKnights == 0 &&
        ((theirBishops == 0 && theirKnights == 1 && ourPawnCount > theirPawnCount) ||
         (theirBishops == 1 && theirKnights == 1 && ourPawnCount >= theirPawnCount + 2) ||
         (ourQueens == 0 && theirBishops == 1 && theirKnights == 0 && ourPawnCount > theirPawnCount))) {
        score += S(65, 65);
    }
    if (ourRooks < theirRooks && ourMinors > theirMinors) {
        int minorAdvantage = ourMinors - theirMinors;
        int rookDeficit = theirRooks - ourRooks;
        if (minorAdvantage >= rookDeficit) {
            score.eg += MinorForExchange.eg * std::min(minorAdvantage, 2);
        }
    }

    if (ourMinors + ourRooks + ourQueens == 0 && theirMinors + theirRooks + theirQueens == 0) {
        constexpr Bitboard KSMask = 0xE0E0E0E0E0E0E0E0ULL;
        constexpr Bitboard QSMask = 0x0F0F0F0F0F0F0F0FULL;
        Bitboard ourPawns = board.pieces(c, PAWN);
        Bitboard theirPawns = board.pieces(enemy, PAWN);

        Bitboard foldOur = ourPawns | (ourPawns >> 32);
        foldOur |= (foldOur >> 16);
        foldOur |= (foldOur >> 8);
        uint8_t ourFiles = static_cast<uint8_t>(foldOur & 0xFF);

        Bitboard foldTheir = theirPawns | (theirPawns >> 32);
        foldTheir |= (foldTheir >> 16);
        foldTheir |= (foldTheir >> 8);
        uint8_t theirFiles = static_cast<uint8_t>(foldTheir & 0xFF);

        int ourKSFiles = popcount(Bitboard(ourFiles & 0xE0));
        int theirKSFiles = popcount(Bitboard(theirFiles & 0xE0));
        int ourQSFiles = popcount(Bitboard(ourFiles & 0x0F));
        int theirQSFiles = popcount(Bitboard(theirFiles & 0x0F));

        int ourKSPawns = popcount(ourPawns & KSMask);
        int theirKSPawns = popcount(theirPawns & KSMask);
        int ourQSPawns = popcount(ourPawns & QSMask);
        int theirQSPawns = popcount(theirPawns & QSMask);

        Bitboard advRanks = (c == WHITE) ? (RANK_5_BB | RANK_6_BB | RANK_7_BB)
                                         : (RANK_4_BB | RANK_3_BB | RANK_2_BB);

        if (ourKSPawns > theirKSPawns && ourKSFiles >= theirKSFiles && theirQSFiles <= ourQSFiles) {
            score.eg += 75;
            if (ourPawns & KSMask & advRanks) {
                score.eg += 45;
            }
        }
        if (ourQSPawns > theirQSPawns && ourQSFiles >= theirQSFiles && theirKSFiles <= ourKSFiles) {
            score.eg += 75;
            if (ourPawns & QSMask & advRanks) {
                score.eg += 45;
            }
        }
    }

    return score;
}

EvalScore eval_pawn_levers(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    Color enemy = ~c;

    Bitboard ourPawns = board.pieces(c, PAWN);
    Bitboard theirPawns = board.pieces(enemy, PAWN);
    Square enemyKingSq = ctx.kingSquare[enemy];
    File enemyKingFile = file_of(enemyKingSq);

    Rank enemyKingRank = enemy == WHITE ? rank_of(enemyKingSq) : Rank(RANK_8 - rank_of(enemyKingSq));
    if (enemyKingRank > RANK_2) return score;

    Bitboard bb = ourPawns;
    while (bb) {
        Square sq = pop_lsb(bb);
        File pawnFile = file_of(sq);

        int fileDistance = std::abs(pawnFile - enemyKingFile);
        if (fileDistance > 2) continue;

        Bitboard pawnAttacks = pawn_attacks_bb(c, sq);
        if (pawnAttacks & theirPawns) {
            if (fileDistance <= 1) {
                score += PawnLeverOnKingFile;
            } else {
                score += PawnLeverBonus;
            }
        }

        Square advanceSq = c == WHITE ? Square(sq + 8) : Square(sq - 8);
        if (advanceSq >= SQ_A1 && advanceSq <= SQ_H8) {
            if (board.piece_on(advanceSq) == NO_PIECE) {
                Bitboard advancedAttacks = pawn_attacks_bb(c, advanceSq);
                if (advancedAttacks & theirPawns) {
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

EvalScore eval_minor_coordination(const Board& board, Color c, EvalContext& ctx) {
    EvalScore score;
    [[maybe_unused]] Color enemy = ~c;

    Bitboard knights = board.pieces(c, KNIGHT);
    Bitboard bishops = board.pieces(c, BISHOP);
    Bitboard queens = board.pieces(c, QUEEN);

    int knightCount = popcount(knights);
    int bishopCount = popcount(bishops);
    int queenCount = popcount(queens);

    if (queenCount >= 1 && knightCount >= 1) {
        score += QueenKnightSynergy;
    }

    if (bishopCount >= 1 && knightCount >= 1) {
        Bitboard bishopBB = bishops;
        while (bishopBB) {
            Square bSq = pop_lsb(bishopBB);
            bool isLightBishop = ((file_of(bSq) + rank_of(bSq)) % 2) == 1;

            Bitboard knightBB = knights;
            while (knightBB) {
                Square nSq = pop_lsb(knightBB);
                Bitboard knightMoves = knight_attacks_bb(nSq);

                Bitboard sameColor = isLightBishop ?
                    0x55AA55AA55AA55AAULL : 0xAA55AA55AA55AA55ULL;
                int coordSquares = popcount(knightMoves & sameColor);

                if (coordSquares >= 2) {
                    score += BishopKnightCoordination;
                    break;
                }
            }
        }
    }

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

            if (distance <= 3) {
                score += KnightProximityBonus;
            }
        }
    }

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

        if (hasLight && hasDark) {
            score += BishopColorComplex;
        }
    }

    return score;
}

int eval_mop_up(const Board& board, Color c, const EvalContext& ctx, int phase) {
    Color enemy = ~c;
    Square enemyKingSq = ctx.kingSquare[enemy];
    Bitboard enemyAll = board.pieces(enemy);
    bool enemyBareKing = (enemyAll == square_bb(enemyKingSq));

    int ourMat = popcount(board.pieces(c, PAWN)) * PawnValue.eg +
                 popcount(board.pieces(c, KNIGHT)) * KnightValue.eg +
                 popcount(board.pieces(c, BISHOP)) * BishopValue.eg +
                 popcount(board.pieces(c, ROOK)) * RookValue.eg +
                 popcount(board.pieces(c, QUEEN)) * QueenValue.eg;
    int theirMat = popcount(board.pieces(enemy, PAWN)) * PawnValue.eg +
                   popcount(board.pieces(enemy, KNIGHT)) * KnightValue.eg +
                   popcount(board.pieces(enemy, BISHOP)) * BishopValue.eg +
                   popcount(board.pieces(enemy, ROOK)) * RookValue.eg +
                   popcount(board.pieces(enemy, QUEEN)) * QueenValue.eg;

    int matDiff = ourMat - theirMat;
    if (!enemyBareKing && (phase > 8 || matDiff < 300 || board.pieces(enemy, PAWN) != 0)) {
        return 0;
    }
    if (ourMat < RookValue.eg && popcount(board.pieces(c, PAWN)) == 0) {
        return 0;
    }

    Square ourKingSq = ctx.kingSquare[c];
    int ekFile = file_of(enemyKingSq);
    int ekRank = rank_of(enemyKingSq);
    int okFile = file_of(ourKingSq);
    int okRank = rank_of(ourKingSq);

    // 1. Push enemy King to edge and corner
    int fileDistFromCenter = std::max(3 - ekFile, ekFile - 4);
    int rankDistFromCenter = std::max(3 - ekRank, ekRank - 4);
    int centerDist = fileDistFromCenter + rankDistFromCenter;
    int mopUp = centerDist * 45 + (fileDistFromCenter * rankDistFromCenter) * 15;

    // 2. Bring our King close to the enemy King (opposition & mating support)
    // Chebyshev distance is the true number of King moves between the two Kings.
    int manhattanDist = std::abs(okFile - ekFile) + std::abs(okRank - ekRank);
    int chebyshevDist = std::max(std::abs(okFile - ekFile), std::abs(okRank - ekRank));
    mopUp += (7 - chebyshevDist) * 45 + (14 - manhattanDist) * 10;

    // Direct opposition / knight-opposition bonus when enemy king is near the edge
    if (centerDist >= 3 && chebyshevDist == 2) {
        mopUp += 40;
    }

    // 3. Restrict enemy King's safe escape squares & cut off ranks/files
    if (enemyBareKing) {
        Bitboard enemyKingMoves = king_attacks_bb(enemyKingSq) & ~ctx.attackedBy[c][ALL_PIECES];
        int safeKingMoves = popcount(enemyKingMoves);
        mopUp += (8 - safeKingMoves) * 22;

        // Bonus for Rooks/Queens cutting off adjacent ranks/files toward the center
        Bitboard majors = board.pieces(c, ROOK, QUEEN);
        while (majors) {
            Square mSq = pop_lsb(majors);
            if (!(ctx.attackedBy[enemy][KING] & square_bb(mSq)) ||
                (ctx.attackedBy[c][ALL_PIECES] & square_bb(mSq))) {
                int fDiff = std::abs(int(file_of(mSq)) - ekFile);
                int rDiff = std::abs(int(rank_of(mSq)) - ekRank);
                if (fDiff == 1 || rDiff == 1) {
                    mopUp += 20;
                }
            }
        }

        // 4. KBN vs K: Drive enemy King to the corner matching our Bishop's color
        int ourBishops = popcount(board.pieces(c, BISHOP));
        int ourKnights = popcount(board.pieces(c, KNIGHT));
        int ourMajors = popcount(board.pieces(c, ROOK, QUEEN));
        if (ourBishops == 1 && ourKnights >= 1 && ourMajors == 0) {
            Square bSq = lsb(board.pieces(c, BISHOP));
            bool lightBishop = ((file_of(bSq) + rank_of(bSq)) % 2) == 1;
            int distCorner1, distCorner2;
            if (lightBishop) {
                // Light corners: A8 (0,7) and H1 (7,0)
                distCorner1 = ekFile + (7 - ekRank);
                distCorner2 = (7 - ekFile) + ekRank;
            } else {
                // Dark corners: A1 (0,0) and H8 (7,7)
                distCorner1 = ekFile + ekRank;
                distCorner2 = (7 - ekFile) + (7 - ekRank);
            }
            int minCornerDist = std::min(distCorner1, distCorner2);
            mopUp += (7 - minCornerDist) * 65;
        }
    }

    return mopUp;
}

int evaluate(const Board& board, int alpha, int beta) {
    (void)alpha;
    (void)beta;
    PROFILE_SCOPE("Eval::evaluate");
    EvalScore score;
    int phase = 0;
    phase += popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase += popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase += popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase += popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::min(phase, TotalPhase);

    score += board.psqt_score(WHITE);
    score -= board.psqt_score(BLACK);

    Key pawnKey = board.pawn_key();
    PawnEntry* pawnEntry = pawnTable.probe(pawnKey);
    EvalScore pawnScore;

    if (pawnEntry->match(pawnKey)) {
        pawnScore = pawnEntry->score;
    } else {
        Bitboard wPassed = 0, bPassed = 0, wHanging = 0, bHanging = 0;
        pawnScore += eval_pawn_structure(board, WHITE, wPassed, wHanging);
        pawnScore -= eval_pawn_structure(board, BLACK, bPassed, bHanging);

        pawnEntry->key = pawnKey;
        pawnEntry->score = pawnScore;
        pawnEntry->passedPawns[WHITE] = wPassed;
        pawnEntry->passedPawns[BLACK] = bPassed;
        pawnEntry->hangingPawns[WHITE] = wHanging;
        pawnEntry->hangingPawns[BLACK] = bHanging;
    }
    score += pawnScore;
    score += eval_passed_pawns_dynamic(board, WHITE, pawnEntry->passedPawns[WHITE], pawnEntry->hangingPawns[WHITE]);
    score -= eval_passed_pawns_dynamic(board, BLACK, pawnEntry->passedPawns[BLACK], pawnEntry->hangingPawns[BLACK]);

    EvalContext ctx;
    init_eval_context(ctx, board);

    score += eval_pieces_with_context(board, WHITE, ctx);
    score -= eval_pieces_with_context(board, BLACK, ctx);

    score += eval_king_safety_with_context(board, WHITE, ctx);
    score -= eval_king_safety_with_context(board, BLACK, ctx);

    score += eval_threats_with_context(board, WHITE, ctx);
    score -= eval_threats_with_context(board, BLACK, ctx);
    score += eval_space(board, WHITE);
    score -= eval_space(board, BLACK);

    score += eval_material_imbalance(board, WHITE);
    score -= eval_material_imbalance(board, BLACK);

    score += eval_pawn_levers(board, WHITE, ctx);
    score -= eval_pawn_levers(board, BLACK, ctx);

    score += eval_minor_coordination(board, WHITE, ctx);
    score -= eval_minor_coordination(board, BLACK, ctx);

    int mg = score.mg;
    int eg = score.eg;
    int finalScore = (mg * phase + eg * (TotalPhase - phase)) / TotalPhase;

    finalScore += eval_mop_up(board, WHITE, ctx, phase);
    finalScore -= eval_mop_up(board, BLACK, ctx, phase);

    constexpr int TEMPO = 12;
    return (board.side_to_move() == WHITE ? finalScore : -finalScore) + TEMPO;
}

int evaluate(const Board& board) {
    return evaluate(board, -30000, 30000);
}

int material_balance(const Board& board) {
    int balance = 0;

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

void print_eval(const Board& board) {
    int phase = 0;
    phase += popcount(board.pieces(KNIGHT)) * PhaseValue[KNIGHT];
    phase += popcount(board.pieces(BISHOP)) * PhaseValue[BISHOP];
    phase += popcount(board.pieces(ROOK)) * PhaseValue[ROOK];
    phase += popcount(board.pieces(QUEEN)) * PhaseValue[QUEEN];
    phase = std::min(phase, TotalPhase);

    EvalScore psqt = board.psqt_score(WHITE) - board.psqt_score(BLACK);

    Key pawnKey = board.pawn_key();
    Bitboard wPassed = 0, bPassed = 0, wHanging = 0, bHanging = 0;
    EvalScore pawnScore = eval_pawn_structure(board, WHITE, wPassed, wHanging) -
                          eval_pawn_structure(board, BLACK, bPassed, bHanging);
    EvalScore passedScore = eval_passed_pawns_dynamic(board, WHITE, wPassed, wHanging) -
                            eval_passed_pawns_dynamic(board, BLACK, bPassed, bHanging);

    EvalContext ctx;
    init_eval_context(ctx, board);

    EvalScore pieces = eval_pieces_with_context(board, WHITE, ctx) - eval_pieces_with_context(board, BLACK, ctx);
    EvalScore king = eval_king_safety_with_context(board, WHITE, ctx) - eval_king_safety_with_context(board, BLACK, ctx);
    EvalScore threats = eval_threats_with_context(board, WHITE, ctx) - eval_threats_with_context(board, BLACK, ctx);
    EvalScore space = eval_space(board, WHITE) - eval_space(board, BLACK);
    EvalScore imbalance = eval_material_imbalance(board, WHITE) - eval_material_imbalance(board, BLACK);
    EvalScore levers = eval_pawn_levers(board, WHITE, ctx) - eval_pawn_levers(board, BLACK, ctx);
    EvalScore coord = eval_minor_coordination(board, WHITE, ctx) - eval_minor_coordination(board, BLACK, ctx);

    EvalScore total = psqt + pawnScore + passedScore + pieces + king + threats + space + imbalance + levers + coord;
    int finalScore = (total.mg * phase + total.eg * (TotalPhase - phase)) / TotalPhase;
    int mopUp = eval_mop_up(board, WHITE, ctx, phase) - eval_mop_up(board, BLACK, ctx, phase);
    finalScore += mopUp;
    constexpr int TEMPO = 12;
    int povScore = (board.side_to_move() == WHITE ? finalScore : -finalScore) + TEMPO;

    std::cout << "--- Eval Breakdown (White perspective) ---" << std::endl;
    std::cout << "Phase: " << phase << "/" << TotalPhase << std::endl;
    std::cout << "PSQT:      mg=" << psqt.mg << " eg=" << psqt.eg << std::endl;
    std::cout << "Pawns:     mg=" << pawnScore.mg << " eg=" << pawnScore.eg << std::endl;
    std::cout << "Passed:    mg=" << passedScore.mg << " eg=" << passedScore.eg << std::endl;
    std::cout << "Pieces:    mg=" << pieces.mg << " eg=" << pieces.eg << std::endl;
    std::cout << "KingSafe:  mg=" << king.mg << " eg=" << king.eg << std::endl;
    std::cout << "Threats:   mg=" << threats.mg << " eg=" << threats.eg << std::endl;
    std::cout << "Space:     mg=" << space.mg << " eg=" << space.eg << std::endl;
    std::cout << "Imbalance: mg=" << imbalance.mg << " eg=" << imbalance.eg << std::endl;
    std::cout << "Levers:    mg=" << levers.mg << " eg=" << levers.eg << std::endl;
    std::cout << "Coord:     mg=" << coord.mg << " eg=" << coord.eg << std::endl;
    std::cout << "Total:     mg=" << total.mg << " eg=" << total.eg << std::endl;
    std::cout << "MopUp:     " << mopUp << std::endl;
    std::cout << "Final:     " << finalScore << " (POV: " << povScore << ")" << std::endl;
}

}

