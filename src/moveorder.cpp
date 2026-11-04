#include "moveorder.hpp"
#include "movegen.hpp"
#include "magic.hpp"
#include "profiler.hpp"
#include "optimize.hpp"

inline bool is_aligned(Square s1, Square s2, Square s3) {
    return (LineBB[s1][s2] & square_bb(s3)) != 0;
}

PieceType SEE::min_attacker(const Board& board, Color side, Square sq,
                            Bitboard occupied, Bitboard& attackers) {
    for (PieceType pt = PAWN; pt <= KING; ++pt) {
        Bitboard bb;

        switch (pt) {
            case PAWN:
                bb = pawn_attacks_bb(~side, sq) & board.pieces(side, PAWN);
                break;
            case KNIGHT:
                bb = knight_attacks_bb(sq) & board.pieces(side, KNIGHT);
                break;
            case BISHOP:
                bb = bishop_attacks_bb(sq, occupied) & board.pieces(side, BISHOP);
                break;
            case ROOK:
                bb = rook_attacks_bb(sq, occupied) & board.pieces(side, ROOK);
                break;
            case QUEEN:
                bb = queen_attacks_bb(sq, occupied) & board.pieces(side, QUEEN);
                break;
            case KING:
                bb = king_attacks_bb(sq) & board.pieces(side, KING);
                break;
            default:
                bb = EMPTY_BB;
                break;
        }

        bb &= occupied;

        if (bb) {
            attackers = bb;
            return pt;
        }
    }

    return NO_PIECE_TYPE;
}

int SEE::evaluate(const Board& board, Move m) {
    PROFILE_SCOPE("SEE::evaluate");
    Square from = m.from();
    Square to = m.to();

    PieceType attacker = type_of(board.piece_on(from));
    PieceType victim = type_of(board.piece_on(to));

    if (m.is_enpassant()) {
        victim = PAWN;
    }

    if (victim == NO_PIECE_TYPE && !m.is_enpassant() && !m.is_promotion()) {
        return 0;
    }

    int gain[32];
    int depth = 0;

    gain[depth] = PieceValue[victim];

    if (m.is_promotion()) {
        PieceType promoPt = m.promotion_type();
        gain[depth] += PieceValue[promoPt] - PieceValue[PAWN];
        attacker = promoPt;
    }

    Bitboard occupied = board.pieces();
    occupied ^= square_bb(from);
    occupied |= square_bb(to);

    if (m.is_enpassant()) {
        Square ep_sq = to - pawn_push(board.side_to_move());
        occupied ^= square_bb(ep_sq);
    }

    Color side = ~board.side_to_move();
    Bitboard attackers_bb;

    while (true) {
        ++depth;
        gain[depth] = PieceValue[attacker] - gain[depth - 1];

        // Jangan prune dengan "max(-gain[d-1], gain[d]) < 0": prune itu
        // keliru ketika pihak berikutnya masih punya recapture yang
        // MENGURANGI kerugian (mis. Bxe5 fxe5 dxe5: -130, bukan -230).
        // Batasi hanya oleh kapasitas gain[].
        if (depth >= 31) {
            break;
        }

        attacker = min_attacker(board, side, to, occupied, attackers_bb);

        if (attacker == NO_PIECE_TYPE) {
            break;
        }

        Square attacker_sq = lsb(attackers_bb);
        occupied ^= square_bb(attacker_sq);

        side = ~side;
    }

    while (--depth) {
        gain[depth - 1] = -std::max(-gain[depth - 1], gain[depth]);
    }

    return gain[0];
}

bool SEE::see_ge(const Board& board, Move m, int threshold) {
    if (m.is_castling()) {
        return threshold <= 0;
    }

    Square from = m.from();
    Square to = m.to();

    PieceType attacker = type_of(board.piece_on(from));
    PieceType victim = type_of(board.piece_on(to));

    if (m.is_enpassant()) {
        victim = PAWN;
    }

    int swap = PieceValue[victim] - threshold;

    if (m.is_promotion()) {
        swap += PieceValue[m.promotion_type()] - PieceValue[PAWN];
    }

    if (swap < 0) {
        return false;
    }

    int attackerValue = m.is_promotion() ? PieceValue[m.promotion_type()] : PieceValue[attacker];
    swap = attackerValue - swap;
    if (swap <= 0) {
        return true;
    }

    return evaluate(board, m) >= threshold;
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, int p,
                       const KillerTable& kt, const CounterMoveTable& cm,
                       const HistoryTable& ht, Move prevMove,
                       const ContinuationHistoryEntry* contHist1,
                       const ContinuationHistoryEntry* contHist2,
                       const CaptureHistory* ch,
                       const ContinuationHistoryEntry* contHist3,
                       const ContinuationHistoryEntry* contHist4)
    : board(b), history(ht), killers(&kt), counterMoves(&cm),
      contHist1ply(contHist1), contHist2ply(contHist2), contHist3ply(contHist3), contHist4ply(contHist4),
      captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), quietCheckCount(0), currentIdx(0), equalCaptureIdx(0), quietCheckIdx(0),
      badCaptureIdx(0), ply(p), stage(STAGE_TT_MOVE),
      hasMoveInfo(false), givesCheckInfo(false), attacksKingZoneInfo(false),
      hasSeeZero(false), seeZeroInfo(false), lastFlags(0) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }

    killer1 = kt.get(p, 0);
    killer2 = kt.get(p, 1);

    if (prevMove) {
        Piece pc = b.piece_on(prevMove.to());
        counterMove = cm.get(pc, prevMove.to());
    } else {
        counterMove = MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, const HistoryTable& ht)
    : board(b), history(ht), killers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr), contHist3ply(nullptr), contHist4ply(nullptr), captureHist(nullptr),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0), ply(0),
      stage(STAGE_QS_TT_MOVE),
      hasMoveInfo(false), givesCheckInfo(false), attacksKingZoneInfo(false),
      hasSeeZero(false), seeZeroInfo(false), lastFlags(0) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, const HistoryTable& ht,
                       const CaptureHistory* ch)
    : board(b), history(ht), killers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr), contHist3ply(nullptr), contHist4ply(nullptr), captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0), ply(0),
      stage(STAGE_QS_TT_MOVE),
      hasMoveInfo(false), givesCheckInfo(false), attacksKingZoneInfo(false),
      hasSeeZero(false), seeZeroInfo(false), lastFlags(0) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }
}

bool MovePicker::is_tt_move(Move m) const {
    for (int i = 0; i < ttMoveCount; ++i) {
        if (ttMoves[i] == m) return true;
    }
    return false;
}

bool MovePicker::is_quiet_check(Move m) const {
    for (int i = 0; i < quietCheckCount; ++i) {
        if (quietCheckMoves[i] == m) return true;
    }
    return false;
}

void MovePicker::score_captures() {
    PROFILE_SCOPE("score_captures");

    const int moveCount = static_cast<int>(moves.size());
    Color us = board.side_to_move();
    Square enemyKingSq = board.king_square(~us);
    Bitboard enemyKingZone = king_attacks_bb(enemyKingSq) | square_bb(enemyKingSq);
    Bitboard ourAdvPawns = board.pieces(us, PAWN) & ((us == WHITE) ? (RANK_6_BB | RANK_7_BB) : (RANK_3_BB | RANK_2_BB));
    Bitboard advPawnBlockSq = (us == WHITE) ? (ourAdvPawns << 8) : (ourAdvPawns >> 8);
    if (ourAdvPawns) {
        advPawnBlockSq |= (us == WHITE) ? ((ourAdvPawns & ~FILE_A_BB) << 7) | ((ourAdvPawns & ~FILE_H_BB) << 9)
                                        : ((ourAdvPawns & ~FILE_H_BB) >> 7) | ((ourAdvPawns & ~FILE_A_BB) >> 9);
    }

    for (int idx = 0; idx < moveCount; ++idx) {
        auto& sm = moves[idx];
        Move m = sm.move;

        if (idx + 2 < moveCount) {
            PREFETCH_READ(&moves[idx + 2]);
        }

        bool givesCheck = MoveGen::gives_check(board, m);
        flags[idx] = givesCheck ? FLAG_GIVES_CHECK : 0;

        if (m.is_promotion()) {
            PieceType promo = m.promotion_type();
            Piece captured = board.piece_on(m.to());
            int captureBonus = (captured != NO_PIECE) ? PieceValue[type_of(captured)] : 0;

            switch (promo) {
                case QUEEN:  sm.score = SCORE_QUEEN_PROMO + captureBonus; break;
                case KNIGHT: sm.score = SCORE_KNIGHT_PROMO + captureBonus; break;
                case ROOK:   sm.score = SCORE_ROOK_PROMO + captureBonus; break;
                default:     sm.score = SCORE_BISHOP_PROMO + captureBonus; break;
            }
            continue;
        }

        Piece captured = board.piece_on(m.to());
        Piece attacker = board.piece_on(m.from());
        PieceType capturedPt = (captured != NO_PIECE) ? type_of(captured) : PAWN;
        PieceType attackerPt = type_of(attacker);

        int mvvLva = mvv_lva(board, m);
        int valueDiff = PieceValue[capturedPt] - PieceValue[attackerPt];
        bool nearKing = (enemyKingZone & square_bb(m.to())) != 0;

        bool likelyGoodCapture = (valueDiff >= 200);

        if (!likelyGoodCapture && givesCheck) {
            if (SEE::see_ge(board, m, 0)) {
                flags[idx] |= FLAG_SEE_GE_ZERO;
                sm.score = SCORE_WINNING_CAP + 15000 + mvvLva + (nearKing ? 5000 : 0);
            } else {
                sm.score = SCORE_EQUAL_CAP + 500000 + mvvLva / 4 + (nearKing ? 2500 : 0);
            }
            if (captureHist && captured != NO_PIECE) {
                sm.score += captureHist->get(attacker, m.to(), capturedPt) / 16;
            }
            sm.score = std::max(sm.score, SCORE_EQUAL_CAP);
            continue;
        }

        if (likelyGoodCapture) {
            sm.score = SCORE_WINNING_CAP + mvvLva + (nearKing ? 5000 : 0);
            flags[idx] |= FLAG_SEE_GE_ZERO;
            if (captureHist && captured != NO_PIECE) {
                sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
            }
        } else {
            if (SEE::see_ge(board, m, 0)) {
                flags[idx] |= FLAG_SEE_GE_ZERO;
                if (std::abs(valueDiff) <= 50 && capturedPt == attackerPt) {
                    sm.score = SCORE_EQUAL_CAP + mvvLva;
                    switch (capturedPt) {
                        case QUEEN:  sm.score += EQUAL_CAP_QUEEN_BONUS;  break;
                        case ROOK:   sm.score += EQUAL_CAP_ROOK_BONUS;   break;
                        case BISHOP: sm.score += EQUAL_CAP_BISHOP_BONUS; break;
                        case KNIGHT: sm.score += EQUAL_CAP_KNIGHT_BONUS; break;
                        default:     sm.score += EQUAL_CAP_PAWN_BONUS;   break;
                    }
                    if (captureHist && captured != NO_PIECE) {
                        sm.score += captureHist->get(attacker, m.to(), capturedPt) / 16;
                    }
                } else {
                    sm.score = SCORE_WINNING_CAP + mvvLva;
                    if (captureHist && captured != NO_PIECE) {
                        sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
                    }
                }
                if (nearKing) {
                    sm.score += 5000;
                }
            } else {
                bool isEnemyAdvPawn = (capturedPt == PAWN && relative_rank(~us, m.to()) >= RANK_6);
                if (capturedPt >= KNIGHT || nearKing || isEnemyAdvPawn ||
                    ((attackerPt == KNIGHT || attackerPt == BISHOP) && SEE::see_ge(board, m, -PieceValue[KNIGHT]))) {
                    sm.score = SCORE_EQUAL_CAP + 500000 + mvvLva / 4 + (nearKing ? 3000 : 0);
                    if (captureHist && captured != NO_PIECE) {
                        sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
                    }
                    sm.score = std::max(sm.score, SCORE_EQUAL_CAP);
                } else {
                    sm.score = SCORE_LOSING_CAP + mvvLva / 8 +
                               ((advPawnBlockSq & square_bb(m.to())) ? 2000 : 0);
                    if (captureHist && captured != NO_PIECE) {
                        sm.score += captureHist->get(attacker, m.to(), capturedPt) / 64;
                    }
                    badCaptures.add(m, sm.score);
                }
            }
        }
    }
}

void MovePicker::score_quiets() {
    PROFILE_SCOPE("score_quiets");
    Color us = board.side_to_move();
    Color them = ~us;
    Square enemyKing = board.king_square(them);
    Bitboard kingZone = king_attacks_bb(enemyKing) | square_bb(enemyKing);
    Bitboard backRanks = (them == BLACK) ? (RANK_7_BB | RANK_8_BB) : (RANK_1_BB | RANK_2_BB);
    Bitboard kingZone2 = kingZone | ((shift<NORTH>(kingZone) | shift<SOUTH>(kingZone) |
                         shift<EAST>(kingZone) | shift<WEST>(kingZone)) & backRanks);
    Bitboard occupied = board.pieces();
    Bitboard enemyKingBlockers = board.blockers_for_king(them);
    Bitboard enemyQueens = board.pieces(them, QUEEN);
    Bitboard enemyRooksQueens = board.pieces(them, ROOK, QUEEN);
    Bitboard enemyNonPawns = board.pieces(them) ^ board.pieces(them, PAWN, KING);
    Bitboard our7thPawns = board.pieces(us, PAWN) & ((us == WHITE) ? RANK_7_BB : RANK_2_BB);
    Bitboard promoBlockSquares = (us == WHITE) ? (our7thPawns << 8) : (our7thPawns >> 8);
    Bitboard ourAdvPassedZone = EMPTY_BB;
    Bitboard our67 = EMPTY_BB;
    if (board.pieces(QUEEN) == 0 && board.pieces(us, KNIGHT) == 0 && popcount(board.pieces(ROOK)) <= 2) {
        Bitboard our567 = board.pieces(us, PAWN) & ((us == WHITE) ? (RANK_5_BB | RANK_6_BB | RANK_7_BB) : (RANK_4_BB | RANK_3_BB | RANK_2_BB));
        our67 = our567 & ((us == WHITE) ? (RANK_6_BB | RANK_7_BB) : (RANK_3_BB | RANK_2_BB));
        if (more_than_one(our567)) {
            Bitboard tmp67 = our67;
            while (tmp67) {
                Square psq = pop_lsb(tmp67);
                ourAdvPassedZone |= king_attacks_bb(psq);
            }
        }
    }
    Bitboard ourQueens = board.pieces(us, QUEEN);
    bool ourQueenThreatened = false;
    if (ourQueens) {
        Square qSq = lsb(ourQueens);
        ourQueenThreatened = (pawn_attacks_bb(us, qSq) & board.pieces(them, PAWN)) ||
                             (knight_attacks_bb(qSq) & board.pieces(them, KNIGHT)) ||
                             (bishop_attacks_bb(qSq, occupied) & board.pieces(them, BISHOP)) ||
                             (rook_attacks_bb(qSq, occupied) & board.pieces(them, ROOK));
    }
    Bitboard ourQAttackers = EMPTY_BB;
    if (enemyQueens) {
        Square eqSq = lsb(enemyQueens);
        ourQAttackers = (pawn_attacks_bb(them, eqSq) & board.pieces(us, PAWN)) |
                        (knight_attacks_bb(eqSq) & board.pieces(us, KNIGHT)) |
                        (bishop_attacks_bb(eqSq, occupied) & board.pieces(us, BISHOP)) |
                        (rook_attacks_bb(eqSq, occupied) & board.pieces(us, ROOK));
    }

    const int moveCount = static_cast<int>(moves.size());

    for (int idx = 0; idx < moveCount; ++idx) {
        auto& sm = moves[idx];
        Move m = sm.move;
        Square from = m.from();
        Square to = m.to();
        Piece pc = board.piece_on(from);
        PieceType pt = type_of(pc);

        if (idx + 2 < moveCount) {
            PREFETCH_READ(&moves[idx + 2]);
        }

        int tacticalBonus = 0;
        uint8_t mFlags = 0;

        bool directCheck = (board.check_squares(pt) & square_bb(to)) != 0;
        bool discCheck = (enemyKingBlockers & square_bb(from)) && !is_aligned(from, to, enemyKing);
        bool givesCheck = directCheck || discCheck;
        bool discHeavy = false;
        Bitboard backRankMinors = board.pieces(them, KNIGHT, BISHOP) & ((them == BLACK) ? RANK_8_BB : RANK_1_BB);
        Bitboard discTargets = enemyRooksQueens | backRankMinors;
        if (!givesCheck && discTargets) {
            Bitboard rAtk = rook_attacks_bb(from, occupied);
            Bitboard friendlyR = rAtk & board.pieces(us, ROOK, QUEEN);
            Bitboard enemyR = rAtk & discTargets;
            if (friendlyR && enemyR) {
                Square fSq = lsb(friendlyR);
                while (enemyR) {
                    Square eSq = pop_lsb(enemyR);
                    if (is_aligned(fSq, from, eSq) && !is_aligned(from, to, eSq)) {
                        discHeavy = true;
                        break;
                    }
                }
            }
            if (!discHeavy) {
                Bitboard bAtk = bishop_attacks_bb(from, occupied);
                Bitboard friendlyB = bAtk & board.pieces(us, BISHOP, QUEEN);
                Bitboard enemyB = bAtk & discTargets;
                if (friendlyB && enemyB) {
                    Square fSq = lsb(friendlyB);
                    while (enemyB) {
                        Square eSq = pop_lsb(enemyB);
                        if (is_aligned(fSq, from, eSq) && !is_aligned(from, to, eSq)) {
                            discHeavy = true;
                            break;
                        }
                    }
                }
            }
        }
        if (discHeavy) {
            mFlags |= FLAG_ATTACKS_KING_ZONE;
            tacticalBonus += 4500;
        }
        if (!givesCheck && m.is_castling()) {
            givesCheck = MoveGen::gives_check(board, m);
        }

        if (promoBlockSquares & square_bb(from)) {
            mFlags |= FLAG_ATTACKS_KING_ZONE;
            tacticalBonus += 6000;
        }
        if ((pt == ROOK || !more_than_one(our67)) && (ourAdvPassedZone & square_bb(to))) {
            mFlags |= FLAG_ATTACKS_KING_ZONE;
            tacticalBonus += 8000;
        }

        if (givesCheck) {
            mFlags |= FLAG_GIVES_CHECK;
            tacticalBonus += 12000;
        } else if (pt == KNIGHT) {
            Bitboard atk = knight_attacks_bb(to);
            if (atk & (kingZone | enemyRooksQueens)) {
                mFlags |= FLAG_ATTACKS_KING_ZONE;
                tacticalBonus += (atk & enemyQueens) ? 6500 : 4500;
            }
            if (ourQAttackers && (atk & ourQAttackers)) {
                tacticalBonus += 5000;
            }
        } else if (pt == PAWN) {
            Bitboard atk = pawn_attacks_bb(us, to);
            if (atk & (kingZone | enemyNonPawns)) {
                mFlags |= FLAG_ATTACKS_KING_ZONE;
                tacticalBonus += (atk & enemyQueens) ? 6500 : 4500;
            }
            if (ourQAttackers && (atk & ourQAttackers)) {
                tacticalBonus += 5000;
            }
            Rank r = relative_rank(us, to);
            if (r == RANK_7) {
                tacticalBonus += 9000;
            } else if (r == RANK_6) {
                tacticalBonus += 3500;
            }
        } else if (pt == BISHOP) {
            Bitboard atk = bishop_attacks_bb(to, occupied ^ square_bb(from));
            if (atk & (kingZone | enemyRooksQueens)) {
                mFlags |= FLAG_ATTACKS_KING_ZONE;
                tacticalBonus += (atk & enemyQueens) ? 6500 : 4500;
            }
            if (ourQAttackers && (atk & ourQAttackers)) {
                tacticalBonus += 5000;
            }
        } else if (pt == ROOK) {
            Bitboard atk = rook_attacks_bb(to, occupied ^ square_bb(from));
            if (atk & (kingZone2 | enemyQueens)) {
                mFlags |= FLAG_ATTACKS_KING_ZONE;
                tacticalBonus += (atk & (kingZone | enemyQueens)) ? 4500 : 3000;
            }
            if (ourQAttackers && (atk & ourQAttackers)) {
                tacticalBonus += 5000;
            }
        } else if (pt == QUEEN) {
            Bitboard atk = queen_attacks_bb(to, occupied ^ square_bb(from));
            if (atk & (kingZone2 | enemyRooksQueens)) {
                mFlags |= FLAG_ATTACKS_KING_ZONE;
                tacticalBonus += (atk & (kingZone | enemyQueens)) ? 6500 : 3500;
            }
            if (ourQueenThreatened) {
                tacticalBonus += 6000;
            }
            if (ourQAttackers && (atk & ourQAttackers)) {
                tacticalBonus += 5000;
            }
        }

        if (!(mFlags & FLAG_ATTACKS_KING_ZONE) && !givesCheck && (pt == ROOK || pt == BISHOP || pt == QUEEN)) {
            Bitboard ray = line_bb(to, enemyKing);
            if (ray) {
                bool sliderMatchesRay = (pt == ROOK) ? (file_of(to) == file_of(enemyKing) || rank_of(to) == rank_of(enemyKing)) :
                                        (pt == BISHOP) ? (file_of(to) != file_of(enemyKing) && rank_of(to) != rank_of(enemyKing)) : true;
                if (sliderMatchesRay) {
                    Bitboard between = between_bb(to, enemyKing);
                    Bitboard blockers = between & (occupied ^ square_bb(from));
                    if (blockers && (blockers & board.pieces(us)) == blockers && popcount(blockers) == 1) {
                        mFlags |= FLAG_ATTACKS_KING_ZONE;
                        tacticalBonus += 4500;
                    }
                }
            }
        }

        flags[idx] = mFlags;

        int histScore = history.get(us, m);
        int cont1 = contHist1ply ? contHist1ply->get(pt, to) : 0;
        int cont2 = contHist2ply ? contHist2ply->get(pt, to) : 0;
        int cont3 = contHist3ply ? contHist3ply->get(pt, to) : 0;
        int cont4 = contHist4ply ? contHist4ply->get(pt, to) : 0;
        int totalHist = 2 * histScore + cont1 + cont2 + cont3 + cont4;

        sm.score = totalHist + tacticalBonus;

        if (m.is_promotion()) {
            PieceType promo = m.promotion_type();
            sm.score += (promo == QUEEN)  ? SCORE_QUEEN_PROMO :
                        (promo == KNIGHT) ? SCORE_KNIGHT_PROMO :
                        (promo == ROOK)   ? SCORE_ROOK_PROMO :
                                            SCORE_BISHOP_PROMO;
        }
    }
}

void MovePicker::score_quiet_checks() {
    Color us = board.side_to_move();

    for (auto& sm : quietChecks) {
        Move m = sm.move;
        Piece pc = board.piece_on(m.from());
        PieceType pt = type_of(pc);

        sm.score = SCORE_QUIET_CHECK;

        int histScore = history.get(us, m);
        sm.score += histScore / 100;

        if (pt == KNIGHT || pt == BISHOP) {
            sm.score += 2000;
        } else if (pt == ROOK || pt == QUEEN) {
            sm.score += 1000;
        } else if (pt == PAWN) {
            sm.score += 3000;
        }
    }
}

Move MovePicker::pick_best() {
    if (currentIdx >= moves.size()) {
        return MOVE_NONE;
    }
    int bestIdx = currentIdx;
    for (int i = currentIdx + 1; i < moves.size(); ++i) {
        if (moves[i].score > moves[bestIdx].score) {
            bestIdx = i;
        }
    }
    if (bestIdx != currentIdx) {
        std::swap(moves[currentIdx], moves[bestIdx]);
        std::swap(flags[currentIdx], flags[bestIdx]);
    }
    return moves[currentIdx++].move;
}

Move MovePicker::next_move() {
    PROFILE_SCOPE("next_move");
    Move m;

    hasMoveInfo = false;
    givesCheckInfo = false;
    attacksKingZoneInfo = false;
    hasSeeZero = false;
    seeZeroInfo = false;
    lastFlags = 0;

    switch (stage) {
        case STAGE_TT_MOVE:
            while (ttMoveIdx < ttMoveCount) {
                m = ttMoves[ttMoveIdx++];
                if (m && MoveGen::is_pseudo_legal(board, m)) {
                    return m;
                }
            }
            ++stage;
            [[fallthrough]];

        case STAGE_GENERATE_CAPTURES:
            MoveGen::generate_captures(board, moves);
            score_captures();
            currentIdx = 0;
            ++stage;
            [[fallthrough]];

        case STAGE_WINNING_CAPTURES:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (is_tt_move(m)) continue;

                if (moves[currentIdx - 1].score < SCORE_EQUAL_CAP) {
                    currentIdx--;
                    break;
                }
                hasMoveInfo = true;
                lastFlags = flags[currentIdx - 1];
                givesCheckInfo = (flags[currentIdx - 1] & FLAG_GIVES_CHECK) != 0;
                attacksKingZoneInfo = false;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_KILLER_1:
            ++stage;
            if (killer1 && !is_tt_move(killer1) &&
                MoveGen::is_pseudo_legal(board, killer1) &&
                board.empty(killer1.to())) {
                return killer1;
            }
            [[fallthrough]];

        case STAGE_KILLER_2:
            ++stage;
            if (killer2 && !is_tt_move(killer2) &&
                MoveGen::is_pseudo_legal(board, killer2) &&
                board.empty(killer2.to())) {
                return killer2;
            }
            [[fallthrough]];

        case STAGE_COUNTER_MOVE:
            ++stage;
            if (counterMove && !is_tt_move(counterMove) &&
                counterMove != killer1 && counterMove != killer2 &&
                MoveGen::is_pseudo_legal(board, counterMove) &&
                board.empty(counterMove.to())) {
                return counterMove;
            }
            [[fallthrough]];

        case STAGE_GENERATE_QUIETS:
            moves.clear();
            MoveGen::generate_quiets(board, moves);
            score_quiets();
            currentIdx = 0;
            badCaptureIdx = 0;
            ++stage;
            [[fallthrough]];

        case STAGE_QUIETS:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (is_tt_move(m) || m == killer1 || m == killer2 || m == counterMove) {
                    continue;
                }

                hasMoveInfo = true;
                lastFlags = flags[currentIdx - 1];
                givesCheckInfo = (flags[currentIdx - 1] & FLAG_GIVES_CHECK) != 0;
                attacksKingZoneInfo = (flags[currentIdx - 1] & FLAG_ATTACKS_KING_ZONE) != 0;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_BAD_CAPTURES:
            while (badCaptureIdx < badCaptures.size()) {
                m = badCaptures.pick_best(badCaptureIdx++);
                if (is_tt_move(m)) continue;
                hasMoveInfo = true;
                givesCheckInfo = MoveGen::gives_check(board, m);
                Square enemyKingSq = board.king_square(~board.side_to_move());
                Bitboard kZone = king_attacks_bb(enemyKingSq) | square_bb(enemyKingSq);
                Piece movedPc = board.piece_on(m.from());
                attacksKingZoneInfo = (attacks_bb(type_of(movedPc), m.to(), board.pieces() ^ square_bb(m.from())) & kZone) != 0;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_DONE:
            return MOVE_NONE;

        case STAGE_QS_TT_MOVE:
            while (ttMoveIdx < ttMoveCount) {
                m = ttMoves[ttMoveIdx++];
                if (m && MoveGen::is_pseudo_legal(board, m)) {
                    return m;
                }
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QS_GENERATE_CAPTURES:
            MoveGen::generate_captures(board, moves);
            score_captures();
            ++stage;
            [[fallthrough]];

        case STAGE_QS_CAPTURES:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (is_tt_move(m)) continue;
                if (moves[currentIdx - 1].score < SCORE_EQUAL_CAP) break;

                hasMoveInfo = true;
                lastFlags = flags[currentIdx - 1];
                givesCheckInfo = (flags[currentIdx - 1] & FLAG_GIVES_CHECK) != 0;
                attacksKingZoneInfo = false;
                hasSeeZero = true;
                seeZeroInfo = (flags[currentIdx - 1] & FLAG_SEE_GE_ZERO) != 0;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QS_DONE:
            return MOVE_NONE;
    }

    return MOVE_NONE;
}
