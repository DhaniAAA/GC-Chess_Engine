#include "moveorder.hpp"
#include "movegen.hpp"
#include "magic.hpp"
#include "profiler.hpp"
#include "optimize.hpp"

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

    if (victim == NO_PIECE_TYPE && !m.is_enpassant()) {
        return 0;
    }

    int gain[32];
    int depth = 0;

    gain[depth] = PieceValue[victim];

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

        if (std::max(-gain[depth - 1], gain[depth]) < 0) {
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
    if (swap < 0) {
        return false;
    }

    swap = PieceValue[attacker] - swap;
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
                       const CaptureHistory* ch)
    : board(b), history(ht), killers(&kt), counterMoves(&cm),
      contHist1ply(contHist1), contHist2ply(contHist2),
      captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), quietCheckCount(0), currentIdx(0), equalCaptureIdx(0), quietCheckIdx(0),
      badCaptureIdx(0), ply(p), stage(STAGE_TT_MOVE) {

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
      contHist1ply(nullptr), contHist2ply(nullptr), captureHist(nullptr),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0), ply(0),
      stage(STAGE_QS_TT_MOVE) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, const HistoryTable& ht,
                       const CaptureHistory* ch)
    : board(b), history(ht), killers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr), captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0), ply(0),
      stage(STAGE_QS_TT_MOVE) {

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

    for (int idx = 0; idx < moveCount; ++idx) {
        auto& sm = moves[idx];
        Move m = sm.move;

        if (idx + 2 < moveCount) {
            PREFETCH_READ(&moves[idx + 2]);
        }

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

        bool likelyGoodCapture = false;
        bool needsSEE = true;

        if (valueDiff >= 200) {
            likelyGoodCapture = true;
            needsSEE = false;
        }
        else if (valueDiff >= -50 && valueDiff <= 50) {
            needsSEE = true;
        }
        else {
            needsSEE = true;
        }

        bool givesCheck = false;
        if (needsSEE || valueDiff < 0) {
            givesCheck = MoveGen::gives_check(board, m);
            if (givesCheck) {
                sm.score = SCORE_WINNING_CAP + 10000 + mvvLva;
            if (captureHist && captured != NO_PIECE) {
                sm.score += captureHist->get(attacker, m.to(), capturedPt) / 100;
            }
            continue;
            }
        }

        if (!needsSEE && likelyGoodCapture) {
            sm.score = SCORE_WINNING_CAP + mvvLva;
        } else {
        int see_value = SEE::evaluate(board, m);

        if (see_value >= 0) {
            if (std::abs(see_value) <= 50 && capturedPt == attackerPt) {
                sm.score = SCORE_EQUAL_CAP + mvvLva;
                switch (capturedPt) {
                    case QUEEN:  sm.score += EQUAL_CAP_QUEEN_BONUS;  break;
                    case ROOK:   sm.score += EQUAL_CAP_ROOK_BONUS;   break;
                    case BISHOP: sm.score += EQUAL_CAP_BISHOP_BONUS; break;
                    case KNIGHT: sm.score += EQUAL_CAP_KNIGHT_BONUS; break;
                    default:     sm.score += EQUAL_CAP_PAWN_BONUS;   break;
                }
            } else {
                sm.score = SCORE_WINNING_CAP + mvvLva;
            }
        } else {
            if (MoveGen::gives_check(board, m)) {
                sm.score = SCORE_WINNING_CAP + 10000 + mvvLva;
            } else {
                sm.score = SCORE_LOSING_CAP + see_value;
                badCaptures.add(m, sm.score);
            }
        }

        if (captureHist && captured != NO_PIECE) {
            sm.score += captureHist->get(attacker, m.to(), capturedPt) / 100;
        }
        }
    }
}

void MovePicker::score_quiets() {
    PROFILE_SCOPE("score_quiets");
    Color us = board.side_to_move();

    const int moveCount = static_cast<int>(moves.size());

    for (int idx = 0; idx < moveCount; ++idx) {
        auto& sm = moves[idx];
        Move m = sm.move;
        Piece pc = board.piece_on(m.from());
        PieceType pt = type_of(pc);
        Square to = m.to();

        if (idx + 2 < moveCount) {
            PREFETCH_READ(&moves[idx + 2]);
        }

        if (m == killer1) {
            sm.score = SCORE_KILLER_1;
        } else if (m == killer2) {
            sm.score = SCORE_KILLER_2;
        } else if (m == counterMove) {
            sm.score = SCORE_COUNTER;
        } else {
            int histScore = history.get(us, m);

            int contHist1Score = 0;
            int contHist2Score = 0;

            if (contHist1ply) {
                contHist1Score = contHist1ply->get(pt, to);
            }

            if (contHist2ply) {
                contHist2Score = contHist2ply->get(pt, to);
            }

            sm.score = histScore + 2 * contHist1Score + contHist2Score;
        }

        if (pt == QUEEN || pt == ROOK) {
            Square enemyKingSq = board.king_square(~us);
            Bitboard kingZone = king_attacks_bb(enemyKingSq);
            Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
            Bitboard attacksAfter = attacks_bb(pt, to, newOccupied);

            if (attacksAfter & (kingZone | square_bb(enemyKingSq))) {
                sm.score += 5000;
            }
        }

        if (m.is_promotion()) {
            PieceType promo = m.promotion_type();
            if (promo == QUEEN) {
                sm.score += SCORE_QUEEN_PROMO;
            } else if (promo == KNIGHT) {
                sm.score += SCORE_KNIGHT_PROMO;
            } else if (promo == ROOK) {
                sm.score += SCORE_ROOK_PROMO;
            } else {
                sm.score += SCORE_BISHOP_PROMO;
            }
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
    return moves.pick_best(currentIdx++);
}

Move MovePicker::next_move() {
    PROFILE_SCOPE("next_move");
    Move m;

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

                if (moves[currentIdx - 1].score <= SCORE_EQUAL_CAP + EQUAL_CAP_QUEEN_BONUS) {
                    break;
                }
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_GENERATE_QUIET_CHECKS:
            {
                quietChecks.clear();
                quietCheckCount = 0;
                MoveGen::generate_checking_moves(board, quietChecks);

                MoveList filteredChecks;
                for (size_t i = 0; i < quietChecks.size(); ++i) {
                    Move qm = quietChecks[i].move;
                    if (!is_tt_move(qm)) {
                        filteredChecks.add(qm, 0);
                        if (quietCheckCount < MAX_QUIET_CHECKS) {
                            quietCheckMoves[quietCheckCount++] = qm;
                        }
                    }
                }
                quietChecks = filteredChecks;

                score_quiet_checks();
                quietCheckIdx = 0;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QUIET_CHECKS:
            while (quietCheckIdx < quietChecks.size()) {
                m = quietChecks[quietCheckIdx++].move;
                if (is_tt_move(m)) continue;
                if (m == killer1 || m == killer2 || m == counterMove) continue;
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
            equalCaptures.clear();
            for (size_t i = currentIdx; i < moves.size(); ++i) {
                if (moves[i].score >= SCORE_EQUAL_CAP &&
                    moves[i].score < SCORE_WINNING_CAP &&
                    !is_tt_move(moves[i].move)) {
                    equalCaptures.add(moves[i].move, moves[i].score);
                }
            }

            moves.clear();
            MoveGen::generate_quiets(board, moves);
            score_quiets();
            currentIdx = 0;
            equalCaptureIdx = 0;
            badCaptureIdx = 0;
            ++stage;
            [[fallthrough]];

        case STAGE_EQUAL_CAPTURES:
            while (equalCaptureIdx < equalCaptures.size()) {
                m = equalCaptures[equalCaptureIdx++].move;
                if (is_tt_move(m)) continue;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QUIETS:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (is_tt_move(m) || m == killer1 || m == killer2 || m == counterMove) {
                    continue;
                }
                if (is_quiet_check(m)) continue;

                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_BAD_CAPTURES:
            while (badCaptureIdx < badCaptures.size()) {
                m = badCaptures[badCaptureIdx++].move;
                if (is_tt_move(m)) continue;
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
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QS_DONE:
            return MOVE_NONE;
    }

    return MOVE_NONE;
}
