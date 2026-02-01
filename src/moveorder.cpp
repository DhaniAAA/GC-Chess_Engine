#include "moveorder.hpp"
#include "movegen.hpp"
#include "magic.hpp"
#include "profiler.hpp"
#include "optimize.hpp"
#include <vector>
#include <algorithm>

// Global killer move statistics for debugging
KillerStats g_killerStats;

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
                       const KillerTable& kt, const MateKillerTable& mkt, const CounterMoveTable& cm,
                       const HistoryTable& ht, Move prevMove,
                       const ContinuationHistoryEntry* contHist1,
                       const ContinuationHistoryEntry* contHist2,
                       const ContinuationHistoryEntry* contHist4,
                       const CaptureHistory* ch)
    : board(b), history(ht), killers(&kt), mateKillers(&mkt), counterMoves(&cm),
      contHist1ply(contHist1), contHist2ply(contHist2), contHist4ply(contHist4),
      captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), quietCheckCount(0), currentIdx(0), equalCaptureIdx(0), quietCheckIdx(0),
      badCaptureIdx(0), endGoodQuiets(0), ply(p), stage(STAGE_TT_MOVE) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }

    killer1 = kt.get(p, 0);
    killer2 = kt.get(p, 1);
    mateKiller = mkt.get(p);

    if (prevMove) {
        g_killerStats.prevMove_valid++;
        Piece pc = b.piece_on(prevMove.to());
        counterMove = cm.get(pc, prevMove.to());
        if (counterMove != MOVE_NONE) {
            g_killerStats.counterMove_found++;
        }
    } else {
        g_killerStats.prevMove_none++;
        counterMove = MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, const HistoryTable& ht)
    : board(b), history(ht), killers(nullptr), mateKillers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr), contHist4ply(nullptr), captureHist(nullptr),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      mateKiller(MOVE_NONE), counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0),
      endGoodQuiets(0), ply(0), stage(STAGE_QS_TT_MOVE) {

    for (int i = 0; i < 3; ++i) {
        ttMoves[i] = (i < count) ? tm[i] : MOVE_NONE;
    }
}

MovePicker::MovePicker(const Board& b, const Move* tm, int count, const HistoryTable& ht,
                       const CaptureHistory* ch)
    : board(b), history(ht), killers(nullptr), mateKillers(nullptr), counterMoves(nullptr),
      contHist1ply(nullptr), contHist2ply(nullptr), contHist4ply(nullptr), captureHist(ch),
      ttMoveCount(count), ttMoveIdx(0), killer1(MOVE_NONE), killer2(MOVE_NONE),
      mateKiller(MOVE_NONE), counterMove(MOVE_NONE), quietCheckCount(0), currentIdx(0), badCaptureIdx(0),
      endGoodQuiets(0), ply(0), stage(STAGE_QS_TT_MOVE) {

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
                sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
            }
            continue;
            }
        }

        if (!needsSEE && likelyGoodCapture) {
            sm.score = SCORE_WINNING_CAP + mvvLva;
            if (captureHist && captured != NO_PIECE) {
                sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
            }
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
                if (captureHist && captured != NO_PIECE) {
                    sm.score += captureHist->get(attacker, m.to(), capturedPt) / 16;
                }
            } else {
                sm.score = SCORE_WINNING_CAP + mvvLva;
                if (captureHist && captured != NO_PIECE) {
                    sm.score += captureHist->get(attacker, m.to(), capturedPt) / 32;
                }
            }
        } else {
            if (MoveGen::gives_check(board, m)) {
                sm.score = SCORE_WINNING_CAP + 10000 + mvvLva;
            } else {
                sm.score = SCORE_LOSING_CAP + see_value;
                if (captureHist && captured != NO_PIECE) {
                    sm.score += captureHist->get(attacker, m.to(), capturedPt) / 64;
                }
                badCaptures.add(m, sm.score);
            }
        }
        }
    }
}

// ============================================================================
// Threat-Based Scoring Helper
//
// Computes bitboards of squares threatened by enemy pieces of lesser value.
// Moving TO a threatened square = penalty (piece can be captured by lesser piece)
// Moving FROM a threatened square = bonus (escaping attack by lesser piece)
//
// Example: If White Knight is on e4 and Black Pawn attacks e4:
// - threatByLesser[KNIGHT] includes e4 because pawns attack it
// - Knight moving FROM e4 gets a bonus (escaping pawn's attack)
// - Knight moving TO a pawn-attacked square gets penalty
// ============================================================================

struct ThreatBitboards {
    Bitboard threats[PIECE_TYPE_NB];

    // Pre-compute all threat bitboards for the enemy color
    void compute(const Board& board, Color them) {
        Bitboard occupied = board.pieces();

        // Pawns can't be threatened by lesser pieces (no piece is worth less)
        threats[PAWN] = EMPTY_BB;

        // Knights and Bishops can be threatened by pawns
        Bitboard pawnThreats = pawn_attacks_bb(them, board.pieces(them, PAWN));
        threats[KNIGHT] = pawnThreats;
        threats[BISHOP] = pawnThreats;

        // Rooks can be threatened by pawns, knights, and bishops
        Bitboard knightThreats = EMPTY_BB;
        Bitboard knights = board.pieces(them, KNIGHT);
        while (knights) {
            Square sq = pop_lsb(knights);
            knightThreats |= knight_attacks_bb(sq);
        }

        Bitboard bishopThreats = EMPTY_BB;
        Bitboard bishops = board.pieces(them, BISHOP);
        while (bishops) {
            Square sq = pop_lsb(bishops);
            bishopThreats |= bishop_attacks_bb(sq, occupied);
        }

        threats[ROOK] = pawnThreats | knightThreats | bishopThreats;

        // Queens can be threatened by pawns, knights, bishops, and rooks
        Bitboard rookThreats = EMPTY_BB;
        Bitboard rooks = board.pieces(them, ROOK);
        while (rooks) {
            Square sq = pop_lsb(rooks);
            rookThreats |= rook_attacks_bb(sq, occupied);
        }

        threats[QUEEN] = threats[ROOK] | rookThreats;

        // King can be threatened by all pieces (special case for king safety)
        Bitboard queenThreats = EMPTY_BB;
        Bitboard queens = board.pieces(them, QUEEN);
        while (queens) {
            Square sq = pop_lsb(queens);
            queenThreats |= queen_attacks_bb(sq, occupied);
        }

        threats[KING] = threats[QUEEN] | queenThreats;
    }

    // Debug function to print threat information
    void debug_print(const Board& board) const {
        const char* pieceNames[] = {"None", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};
        const char* fileNames = "abcdefgh";

        std::cout << "\n========== THREAT-BASED SCORING DEBUG ==========\n";
        std::cout << "Side to move: " << (board.side_to_move() == WHITE ? "White" : "Black") << "\n";
        std::cout << "Threats are from: " << (board.side_to_move() == WHITE ? "Black" : "White") << " pieces\n\n";

        for (PieceType pt = KNIGHT; pt <= KING; ++pt) {
            std::cout << pieceNames[pt] << " threatened squares (by lesser pieces): ";

            if (threats[pt] == EMPTY_BB) {
                std::cout << "none";
            } else {
                Bitboard bb = threats[pt];
                bool first = true;
                while (bb) {
                    Square sq = pop_lsb(bb);
                    if (!first) std::cout << ", ";
                    std::cout << fileNames[file_of(sq)] << (rank_of(sq) + 1);
                    first = false;
                }
            }
            std::cout << " (" << popcount(threats[pt]) << " squares)\n";
        }
        std::cout << "\n";
    }
};

// ============================================================================
// Debug Functions Implementation
// ============================================================================

void debug_threats(const Board& board) {
    Color them = ~board.side_to_move();
    ThreatBitboards threatInfo;
    threatInfo.compute(board, them);
    threatInfo.debug_print(board);

    // Also print visual board with threats
    std::cout << "Visual threat map for current side's pieces:\n";
    std::cout << "  +---+---+---+---+---+---+---+---+\n";

    const char* fileNames = "abcdefgh";

    for (Rank r = RANK_8; r >= RANK_1; --r) {
        std::cout << (r + 1) << " |";
        for (File f = FILE_A; f <= FILE_H; ++f) {
            Square sq = make_square(f, r);
            Piece pc = board.piece_on(sq);

            if (pc != NO_PIECE && color_of(pc) == board.side_to_move()) {
                PieceType pt = type_of(pc);
                bool isThreatened = (threatInfo.threats[pt] & square_bb(sq)) != 0;

                char pieceChar = ".PNBRQK"[pt];
                if (isThreatened) {
                    // Mark threatened pieces with lowercase
                    std::cout << " " << (char)tolower(pieceChar) << "!|";
                } else {
                    std::cout << " " << pieceChar << " |";
                }
            } else if (pc != NO_PIECE) {
                char pieceChar = ".pnbrqk"[type_of(pc)];
                std::cout << " " << pieceChar << " |";
            } else {
                std::cout << "   |";
            }
        }
        std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    }
    std::cout << "    a   b   c   d   e   f   g   h\n\n";
    std::cout << "Legend: UPPERCASE = our pieces, lowercase = enemy pieces\n";
    std::cout << "        n! = piece is threatened by lesser piece (escape bonus if moved)\n\n";
}

void debug_move_threat_score(const Board& board, Move m) {
    if (!m) {
        std::cout << "Invalid move\n";
        return;
    }

    Color us = board.side_to_move();
    Color them = ~us;

    ThreatBitboards threatInfo;
    threatInfo.compute(board, them);

    Piece pc = board.piece_on(m.from());
    PieceType pt = type_of(pc);
    Square from = m.from();
    Square to = m.to();

    const char* pieceNames[] = {"None", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};
    const char* fileNames = "abcdefgh";

    std::cout << "\n========== MOVE THREAT SCORE DEBUG ==========\n";
    std::cout << "Move: " << fileNames[file_of(from)] << (rank_of(from) + 1)
              << fileNames[file_of(to)] << (rank_of(to) + 1) << "\n";
    std::cout << "Piece: " << pieceNames[pt] << " (value: " << PieceValue[pt] << ")\n\n";

    bool fromThreatened = (threatInfo.threats[pt] & square_bb(from)) != 0;
    bool toThreatened = (threatInfo.threats[pt] & square_bb(to)) != 0;

    std::cout << "From square " << fileNames[file_of(from)] << (rank_of(from) + 1)
              << " is threatened: " << (fromThreatened ? "YES" : "NO") << "\n";
    std::cout << "To square " << fileNames[file_of(to)] << (rank_of(to) + 1)
              << " is threatened: " << (toThreatened ? "YES" : "NO") << "\n\n";

    int threatAdjustment = 0;

    if (toThreatened) {
        threatAdjustment -= 19;
        std::cout << "PENALTY: Moving TO a threatened square = -19 multiplier\n";
    }

    if (fromThreatened) {
        threatAdjustment += 20;
        std::cout << "BONUS: ESCAPING FROM a threatened square = +20 multiplier\n";
    }

    int totalAdjustment = PieceValue[pt] * threatAdjustment;

    std::cout << "\nCalculation: PieceValue[" << pieceNames[pt] << "] * threatMultiplier\n";
    std::cout << "           = " << PieceValue[pt] << " * " << threatAdjustment << "\n";
    std::cout << "           = " << totalAdjustment << "\n\n";

    if (totalAdjustment > 0) {
        std::cout << "Result: This move gets a BONUS of +" << totalAdjustment << " points\n";
        std::cout << "        (Escaping attack is good!)\n";
    } else if (totalAdjustment < 0) {
        std::cout << "Result: This move gets a PENALTY of " << totalAdjustment << " points\n";
        std::cout << "        (Moving into attack is bad!)\n";
    } else {
        std::cout << "Result: No threat adjustment for this move\n";
    }
    std::cout << "=============================================\n\n";
}

// Debug function to show good/bad quiet split
void debug_quiet_split(const Board& board) {
    // Generate all quiet moves
    MoveList moves;
    MoveGen::generate_quiets(board, moves);

    Color us = board.side_to_move();
    Color them = ~us;

    // Compute threat info
    ThreatBitboards threatInfo;
    threatInfo.compute(board, them);

    // Create a dummy history table for scoring
    HistoryTable history;

    std::cout << "\n========== GOOD/BAD QUIET SPLIT DEBUG ==========\n";
    std::cout << "Side to move: " << (us == WHITE ? "White" : "Black") << "\n";
    std::cout << "GOOD_QUIET_THRESHOLD: " << GOOD_QUIET_THRESHOLD << "\n";
    std::cout << "Total quiet moves: " << moves.size() << "\n\n";

    // Score each move manually
    std::vector<std::pair<Move, int>> scoredMoves;
    const char* fileNames = "abcdefgh";

    for (size_t i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        Piece pc = board.piece_on(m.from());
        PieceType pt = type_of(pc);
        Square from = m.from();
        Square to = m.to();

        // Basic history score (0 for debug since we don't have actual history)
        int histScore = history.get(us, m);

        // Threat adjustment
        int threatAdjustment = 0;
        if (threatInfo.threats[pt] & square_bb(to)) {
            threatAdjustment -= 19;
        }
        if (threatInfo.threats[pt] & square_bb(from)) {
            threatAdjustment += 20;
        }
        histScore += PieceValue[pt] * threatAdjustment;

        scoredMoves.push_back({m, histScore});
    }

    // Sort by score
    std::sort(scoredMoves.begin(), scoredMoves.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Count good vs bad
    int goodCount = 0, badCount = 0;
    for (const auto& sm : scoredMoves) {
        if (sm.second > GOOD_QUIET_THRESHOLD) goodCount++;
        else badCount++;
    }

    std::cout << "GOOD quiets (score > " << GOOD_QUIET_THRESHOLD << "): " << goodCount << "\n";
    std::cout << "BAD quiets (score <= " << GOOD_QUIET_THRESHOLD << "): " << badCount << "\n\n";

    // Show moves
    if (goodCount > 0) {
        std::cout << "--- GOOD QUIETS (searched before bad captures) ---\n";
        int shown = 0;
        for (const auto& sm : scoredMoves) {
            if (sm.second > GOOD_QUIET_THRESHOLD) {
                Move m = sm.first;
                std::cout << "  " << fileNames[file_of(m.from())] << (rank_of(m.from()) + 1)
                          << fileNames[file_of(m.to())] << (rank_of(m.to()) + 1)
                          << " score=" << sm.second << "\n";
                if (++shown >= 10) {
                    std::cout << "  ... and " << (goodCount - shown) << " more\n";
                    break;
                }
            }
        }
        std::cout << "\n";
    }

    if (badCount > 0) {
        std::cout << "--- BAD QUIETS (searched after bad captures) ---\n";
        int shown = 0;
        for (const auto& sm : scoredMoves) {
            if (sm.second <= GOOD_QUIET_THRESHOLD) {
                Move m = sm.first;
                std::cout << "  " << fileNames[file_of(m.from())] << (rank_of(m.from()) + 1)
                          << fileNames[file_of(m.to())] << (rank_of(m.to()) + 1)
                          << " score=" << sm.second << "\n";
                if (++shown >= 10) {
                    std::cout << "  ... and " << (badCount - shown) << " more\n";
                    break;
                }
            }
        }
    }

    std::cout << "================================================\n\n";
}

void MovePicker::score_quiets() {
    PROFILE_SCOPE("score_quiets");
    Color us = board.side_to_move();
    Color them = ~us;

    // Pre-compute threat bitboards once for all moves
    ThreatBitboards threatInfo;
    threatInfo.compute(board, them);

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

        // Priority scoring for special quiet moves
        // Mate Killer has highest priority among quiet moves
        if (m == mateKiller && mateKiller != MOVE_NONE) {
            sm.score = SCORE_MATE_KILLER;
        } else if (m == killer1) {
            sm.score = SCORE_KILLER_1;
        } else if (m == killer2) {
            sm.score = SCORE_KILLER_2;
        } else if (m == counterMove) {
            sm.score = SCORE_COUNTER;
        } else {
            // =========================================================
            // CONSISTENT HISTORY WEIGHTS (matching LMR formula)
            // Formula: 2 * mainHistory + contHist1 + contHist2 + contHist4
            // This ensures move ordering correlates with LMR decisions
            // =========================================================
            int histScore = 2 * history.get(us, m);

            if (contHist1ply) {
                histScore += contHist1ply->get(pt, to);
            }

            if (contHist2ply) {
                histScore += contHist2ply->get(pt, to);
            }

            if (contHist4ply) {
                histScore += contHist4ply->get(pt, to);
            }

            // =========================================================
            // THREAT-BASED SCORING (Stockfish-style)
            //
            // This gives bonus/penalty based on whether a piece is moving
            // to or from a square attacked by a lesser-valued enemy piece.
            //
            // Moving TO a threatened square = BAD (piece can be captured cheaply)
            // Moving FROM a threatened square = GOOD (escaping cheap capture)
            //
            // The adjustment is scaled by piece value to make it proportional:
            // - A Queen escaping rook attack = big bonus
            // - A Knight escaping pawn attack = smaller bonus
            // =========================================================
            Square from = m.from();
            int threatAdjustment = 0;

            // Penalty for moving TO a square threatened by lesser piece
            // Stockfish uses -19 as the penalty multiplier
            if (threatInfo.threats[pt] & square_bb(to)) {
                threatAdjustment -= 19;
            }

            // Bonus for ESCAPING FROM a square threatened by lesser piece
            // Stockfish uses +20 as the bonus multiplier
            if (threatInfo.threats[pt] & square_bb(from)) {
                threatAdjustment += 20;
            }

            // Scale by piece value for proportional impact
            // Higher value pieces get bigger adjustments
            histScore += PieceValue[pt] * threatAdjustment;

            sm.score = histScore;
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
            if (killer1) {
                g_killerStats.killer1_attempts++;
                if (!is_tt_move(killer1) &&
                    MoveGen::is_pseudo_legal(board, killer1) &&
                    board.empty(killer1.to())) {
                    g_killerStats.killer1_legal++;
                    g_killerStats.killer1_hits++;
                    return killer1;
                }
            }
            [[fallthrough]];

        case STAGE_KILLER_2:
            ++stage;
            if (killer2) {
                g_killerStats.killer2_attempts++;
                if (!is_tt_move(killer2) &&
                    MoveGen::is_pseudo_legal(board, killer2) &&
                    board.empty(killer2.to())) {
                    g_killerStats.killer2_legal++;
                    g_killerStats.killer2_hits++;
                    return killer2;
                }
            }
            [[fallthrough]];

        case STAGE_COUNTER_MOVE:
            ++stage;
            if (counterMove) {
                g_killerStats.counter_attempts++;
                if (!is_tt_move(counterMove) &&
                    counterMove != killer1 && counterMove != killer2 &&
                    MoveGen::is_pseudo_legal(board, counterMove) &&
                    board.empty(counterMove.to())) {
                    g_killerStats.counter_legal++;
                    g_killerStats.counter_hits++;
                    return counterMove;
                }
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

            // =========================================================
            // PARTIAL INSERTION SORT for Good/Bad quiet split
            //
            // This partitions quiets into two groups:
            // - Good quiets: score > GOOD_QUIET_THRESHOLD (searched first)
            // - Bad quiets: score <= GOOD_QUIET_THRESHOLD (searched last)
            //
            // We sort only the good quiets to avoid wasted work on bad ones.
            // =========================================================
            endGoodQuiets = 0;
            for (size_t i = 0; i < moves.size(); ++i) {
                if (moves[i].score > GOOD_QUIET_THRESHOLD) {
                    // Swap to front and insertion sort to keep sorted
                    ScoredMove tmp = moves[i];
                    size_t j = endGoodQuiets;
                    while (j > 0 && moves[j - 1].score < tmp.score) {
                        moves[j] = moves[j - 1];
                        --j;
                    }
                    moves[j] = tmp;
                    ++endGoodQuiets;
                }
            }

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

        case STAGE_GOOD_QUIETS:
            // Return good quiets (score > threshold) first
            while (currentIdx < endGoodQuiets) {
                m = moves[currentIdx++].move;
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

        case STAGE_BAD_QUIETS:
            // Return bad quiets (score <= threshold) last
            while (currentIdx < static_cast<int>(moves.size())) {
                // Use pick_best() only for unsorted bad quiets
                m = pick_best();
                if (is_tt_move(m) || m == killer1 || m == killer2 || m == counterMove) {
                    continue;
                }
                if (is_quiet_check(m)) continue;
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
