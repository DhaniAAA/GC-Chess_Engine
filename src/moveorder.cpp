#include "moveorder.hpp"
#include "movegen.hpp"
#include "magic.hpp"

// ============================================================================
// Static Exchange Evaluation (SEE)
// ============================================================================

PieceType SEE::min_attacker(const Board& board, Color side, Square sq,
                            Bitboard occupied, Bitboard& attackers) {
    // Find the least valuable attacker
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
    Square from = m.from();
    Square to = m.to();

    // Get the initial piece values
    PieceType attacker = type_of(board.piece_on(from));
    PieceType victim = type_of(board.piece_on(to));

    if (m.is_enpassant()) {
        victim = PAWN;
    }

    if (victim == NO_PIECE_TYPE && !m.is_enpassant()) {
        return 0;  // Not a capture
    }

    // Initialize gain list
    int gain[32];
    int depth = 0;

    gain[depth] = PieceValue[victim];

    // Set up occupancy - attacker moves from 'from' to 'to'
    Bitboard occupied = board.pieces();
    occupied ^= square_bb(from);  // Remove attacker from original square
    occupied |= square_bb(to);     // Place attacker on target square

    if (m.is_enpassant()) {
        Square ep_sq = to - pawn_push(board.side_to_move());
        occupied ^= square_bb(ep_sq);  // Remove captured pawn
    }

    Color side = ~board.side_to_move();
    Bitboard attackers_bb;

    // Simulate the exchange
    while (true) {
        ++depth;
        gain[depth] = PieceValue[attacker] - gain[depth - 1];

        // Stand pat pruning - if we are ahead even after losing the piece
        if (std::max(-gain[depth - 1], gain[depth]) < 0) {
            break;
        }

        // Find next attacker
        attacker = min_attacker(board, side, to, occupied, attackers_bb);

        if (attacker == NO_PIECE_TYPE) {
            break;
        }

        // Remove the attacker from the board (use LSB of attackers)
        Square attacker_sq = lsb(attackers_bb);
        occupied ^= square_bb(attacker_sq);

        // X-ray attacks: reveal hidden attackers through vacated square
        // Diagonal x-rays (pawn, bishop, queen)
        if (attacker == PAWN || attacker == BISHOP || attacker == QUEEN) {
            Bitboard diag_xrays = bishop_attacks_bb(to, occupied) &
                                  (board.pieces(BISHOP) | board.pieces(QUEEN));
            // These are already considered by min_attacker due to updated occupancy
        }
        // Straight x-rays (rook, queen)
        if (attacker == ROOK || attacker == QUEEN) {
            Bitboard straight_xrays = rook_attacks_bb(to, occupied) &
                                      (board.pieces(ROOK) | board.pieces(QUEEN));
            // These are already considered by min_attacker due to updated occupancy
        }

        side = ~side;
    }

    // Negamax the gain list
    while (--depth) {
        gain[depth - 1] = -std::max(-gain[depth - 1], gain[depth]);
    }

    return gain[0];
}

bool SEE::see_ge(const Board& board, Move m, int threshold) {
    // Quick check for obvious cases
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

    // If we capture more than threshold and opponent can't recapture
    int swap = PieceValue[victim] - threshold;
    if (swap < 0) {
        return false;
    }

    swap = PieceValue[attacker] - swap;
    if (swap <= 0) {
        return true;
    }

    // Need full SEE
    return evaluate(board, m) >= threshold;
}

// ============================================================================
// Move Picker Implementation
// ============================================================================

MovePicker::MovePicker(const Board& b, Move tt, int p,
                       const KillerTable& kt, const CounterMoveTable& cm,
                       const HistoryTable& ht, Move prevMove,
                       const ContinuationHistoryEntry* contHist1,
                       const ContinuationHistoryEntry* contHist2)
    : board(b), history(ht), killers(&kt), counterMoves(&cm),
      contHist1ply(contHist1), contHist2ply(contHist2),
      ttMove(tt), currentIdx(0), ply(p), stage(STAGE_TT_MOVE) {

    killer1 = kt.get(p, 0);
    killer2 = kt.get(p, 1);

    if (prevMove) {
        Piece pc = b.piece_on(prevMove.to());
        counterMove = cm.get(pc, prevMove.to());
    } else {
        counterMove = MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, Move tt, const HistoryTable& ht)
    : board(b), history(ht), killers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr),
      ttMove(tt), killer1(MOVE_NONE), killer2(MOVE_NONE),
      counterMove(MOVE_NONE), currentIdx(0), ply(0),
      stage(STAGE_QS_TT_MOVE) {}

void MovePicker::score_captures() {
    for (auto& sm : moves) {
        Move m = sm.move;

        // Use SEE for good/bad capture separation
        int see_value = SEE::evaluate(board, m);

        if (see_value >= 0) {
            // Good capture: MVV-LVA + SEE bonus
            sm.score = SCORE_WINNING_CAP + mvv_lva(board, m);
        } else {
            // Bad capture: lose material
            sm.score = SCORE_LOSING_CAP + see_value;
            badCaptures.add(m, sm.score);
        }
    }
}

void MovePicker::score_quiets() {
    Color us = board.side_to_move();

    for (auto& sm : moves) {
        Move m = sm.move;
        Piece pc = board.piece_on(m.from());
        PieceType pt = type_of(pc);
        Square to = m.to();

        // Check for killer moves
        if (m == killer1) {
            sm.score = SCORE_KILLER_1;
        } else if (m == killer2) {
            sm.score = SCORE_KILLER_2;
        } else if (m == counterMove) {
            sm.score = SCORE_COUNTER;
        } else {
            // History heuristic (butterfly history)
            int histScore = history.get(us, m);

            // Continuation history scores (1-ply and 2-ply ago)
            // These are weighted and added to the base history score
            int contHist1Score = 0;
            int contHist2Score = 0;

            if (contHist1ply) {
                contHist1Score = contHist1ply->get(pt, to);
            }

            if (contHist2ply) {
                contHist2Score = contHist2ply->get(pt, to);
            }

            // Combine scores: regular history + continuation histories
            // Weight: 1x regular history + 2x cont1 (more relevant) + 1x cont2
            sm.score = histScore + 2 * contHist1Score + contHist2Score;
        }

        // Bonus for promotions
        if (m.is_promotion()) {
            sm.score += SCORE_PROMOTION;
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
    Move m;

    switch (stage) {
        case STAGE_TT_MOVE:
            ++stage;
            if (ttMove && MoveGen::is_pseudo_legal(board, ttMove)) {
                return ttMove;
            }
            [[fallthrough]];

        case STAGE_GENERATE_CAPTURES:
            MoveGen::generate_captures(board, moves);
            score_captures();
            currentIdx = 0;
            ++stage;
            [[fallthrough]];

        case STAGE_GOOD_CAPTURES:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (m == ttMove) continue;
                if (moves[currentIdx - 1].score < SCORE_EQUAL_CAP) {
                    // Switch to killers, save bad captures for later
                    break;
                }
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_KILLER_1:
            ++stage;
            if (killer1 && killer1 != ttMove &&
                MoveGen::is_pseudo_legal(board, killer1) &&
                board.empty(killer1.to())) {
                return killer1;
            }
            [[fallthrough]];

        case STAGE_KILLER_2:
            ++stage;
            if (killer2 && killer2 != ttMove &&
                MoveGen::is_pseudo_legal(board, killer2) &&
                board.empty(killer2.to())) {
                return killer2;
            }
            [[fallthrough]];

        case STAGE_COUNTER_MOVE:
            ++stage;
            if (counterMove && counterMove != ttMove &&
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
            ++stage;
            [[fallthrough]];

        case STAGE_QUIETS:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (m == ttMove || m == killer1 || m == killer2 || m == counterMove) {
                    continue;
                }
                return m;
            }
            currentIdx = 0;
            ++stage;
            [[fallthrough]];

        case STAGE_BAD_CAPTURES:
            while (currentIdx < badCaptures.size()) {
                m = badCaptures[currentIdx++].move;
                if (m == ttMove) continue;
                return m;
            }
            ++stage;
            [[fallthrough]];

        case STAGE_DONE:
            return MOVE_NONE;

        // Quiescence search stages
        case STAGE_QS_TT_MOVE:
            ++stage;
            if (ttMove && MoveGen::is_pseudo_legal(board, ttMove)) {
                return ttMove;
            }
            [[fallthrough]];

        case STAGE_QS_GENERATE_CAPTURES:
            MoveGen::generate_captures(board, moves);
            score_captures();
            ++stage;
            [[fallthrough]];

        case STAGE_QS_CAPTURES:
            while (currentIdx < moves.size()) {
                m = pick_best();
                if (m == ttMove) continue;
                // Only good captures in qsearch
                if (moves[currentIdx - 1].score >= SCORE_EQUAL_CAP) {
                    return m;
                }
            }
            ++stage;
            [[fallthrough]];

        case STAGE_QS_DONE:
            return MOVE_NONE;
    }

    return MOVE_NONE;
}
