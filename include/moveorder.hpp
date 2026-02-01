#ifndef MOVEORDER_HPP
#define MOVEORDER_HPP

#include "board.hpp"
#include "move.hpp"
#include "tt.hpp"
#include "search_constants.hpp"
#include <iostream>
#include <cstdint>

// ============================================================================
// Move Ordering Scores (using int32_t for efficient range)
// Higher scores = searched first
// ============================================================================

constexpr int32_t SCORE_TT_MOVE        = 400000000;
constexpr int32_t SCORE_QUEEN_PROMO    = 300000000;
constexpr int32_t SCORE_KNIGHT_PROMO   = 290000000;
constexpr int32_t SCORE_ROOK_PROMO     = 280000000;
constexpr int32_t SCORE_BISHOP_PROMO   = 270000000;
constexpr int32_t SCORE_WINNING_CAP    = 200000000;
constexpr int32_t SCORE_EQUAL_CAP      = 190000000;
constexpr int32_t SCORE_QUIET_CHECK    = 180000000;
constexpr int32_t SCORE_MATE_KILLER    = 15000000;
constexpr int32_t SCORE_KILLER_1       = 10000000;
constexpr int32_t SCORE_KILLER_2       = 9000000;
constexpr int32_t SCORE_COUNTER        = 8000000;
constexpr int32_t SCORE_HISTORY_MAX    = 7000000;
constexpr int32_t SCORE_LOSING_CAP     = -10000000;

constexpr int32_t EQUAL_CAP_QUEEN_BONUS  = 5000;
constexpr int32_t EQUAL_CAP_ROOK_BONUS   = 3000;
constexpr int32_t EQUAL_CAP_BISHOP_BONUS = 1500;
constexpr int32_t EQUAL_CAP_KNIGHT_BONUS = 1000;
constexpr int32_t EQUAL_CAP_PAWN_BONUS   = 0;

// ============================================================================
// Piece Values for MVV-LVA and SEE
// ============================================================================

constexpr int PieceValue[PIECE_TYPE_NB] = {
    0,      // NO_PIECE_TYPE
    100,    // PAWN
    320,    // KNIGHT
    330,    // BISHOP
    500,    // ROOK
    900,    // QUEEN
    20000   // KING
};

// ============================================================================
// Static Exchange Evaluation (SEE)
//
// Determines if a capture is winning, losing, or equal by simulating
// the sequence of captures on a square.
// ============================================================================

class SEE {
public:
    static int evaluate(const Board& board, Move m);

    static bool see_ge(const Board& board, Move m, int threshold = 0);

private:
    static PieceType min_attacker(const Board& board, Color side, Square sq,
                                  Bitboard occupied, Bitboard& attackers);
};

// ============================================================================
// Threat-Based Scoring Debug
//
// Prints detailed information about which squares are threatened by lesser
// pieces. Useful for understanding and tuning the threat-based move ordering.
// ============================================================================

void debug_threats(const Board& board);
void debug_move_threat_score(const Board& board, Move m);
void debug_quiet_split(const Board& board);

// ============================================================================
// Killer Move Statistics (Debug)
//
// Tracks hit/miss rates for killer moves to determine if they should be
// integrated into quiet scoring like Stockfish does for NNUE.
// ============================================================================

struct KillerStats {
    // Tracking counters
    uint64_t killer1_attempts = 0;   // Times we tried killer1
    uint64_t killer1_legal = 0;      // Times killer1 was pseudo-legal and quiet
    uint64_t killer1_hits = 0;       // Times killer1 caused cutoff (returned by MovePicker)

    uint64_t killer2_attempts = 0;
    uint64_t killer2_legal = 0;
    uint64_t killer2_hits = 0;

    uint64_t counter_attempts = 0;
    uint64_t counter_legal = 0;
    uint64_t counter_hits = 0;

    // Debug: prevMove tracking
    uint64_t prevMove_valid = 0;     // Times prevMove was not MOVE_NONE
    uint64_t prevMove_none = 0;      // Times prevMove was MOVE_NONE
    uint64_t counterMove_found = 0;  // Times we found a counter move in table

    void clear() {
        killer1_attempts = killer1_legal = killer1_hits = 0;
        killer2_attempts = killer2_legal = killer2_hits = 0;
        counter_attempts = counter_legal = counter_hits = 0;
        prevMove_valid = prevMove_none = counterMove_found = 0;
    }

    void print() const {
        std::cout << "\n========== KILLER MOVE STATISTICS ==========\n";

        double k1_legal_rate = killer1_attempts > 0 ?
            100.0 * killer1_legal / killer1_attempts : 0;
        double k1_hit_rate = killer1_legal > 0 ?
            100.0 * killer1_hits / killer1_legal : 0;

        double k2_legal_rate = killer2_attempts > 0 ?
            100.0 * killer2_legal / killer2_attempts : 0;
        double k2_hit_rate = killer2_legal > 0 ?
            100.0 * killer2_hits / killer2_legal : 0;

        double cm_legal_rate = counter_attempts > 0 ?
            100.0 * counter_legal / counter_attempts : 0;
        double cm_hit_rate = counter_legal > 0 ?
            100.0 * counter_hits / counter_legal : 0;

        std::cout << "Killer 1:\n";
        std::cout << "  Attempts: " << killer1_attempts << "\n";
        std::cout << "  Legal:    " << killer1_legal << " (" << k1_legal_rate << "%)\n";
        std::cout << "  Hits:     " << killer1_hits << " (" << k1_hit_rate << "% of legal)\n\n";

        std::cout << "Killer 2:\n";
        std::cout << "  Attempts: " << killer2_attempts << "\n";
        std::cout << "  Legal:    " << killer2_legal << " (" << k2_legal_rate << "%)\n";
        std::cout << "  Hits:     " << killer2_hits << " (" << k2_hit_rate << "% of legal)\n\n";

        std::cout << "Counter Move:\n";
        std::cout << "  Attempts: " << counter_attempts << "\n";
        std::cout << "  Legal:    " << counter_legal << " (" << cm_legal_rate << "%)\n";
        std::cout << "  Hits:     " << counter_hits << " (" << cm_hit_rate << "% of legal)\n\n";

        // Debug: prevMove tracking
        std::cout << "PrevMove Debug:\n";
        std::cout << "  prevMove valid: " << prevMove_valid << "\n";
        std::cout << "  prevMove none:  " << prevMove_none << "\n";
        std::cout << "  counterMove found: " << counterMove_found << "\n\n";

        // Overall assessment
        double overall_legal = (killer1_legal + killer2_legal + counter_legal);
        double overall_attempts = (killer1_attempts + killer2_attempts + counter_attempts);
        double overall_hits = (killer1_hits + killer2_hits + counter_hits);

        double legal_rate = overall_attempts > 0 ? 100.0 * overall_legal / overall_attempts : 0;
        double hit_rate = overall_legal > 0 ? 100.0 * overall_hits / overall_legal : 0;

        std::cout << "OVERALL:\n";
        std::cout << "  Legal rate: " << legal_rate << "%\n";
        std::cout << "  Hit rate:   " << hit_rate << "% (of legal moves)\n\n";

        if (legal_rate < 50.0) {
            std::cout << "RECOMMENDATION: Legal rate is LOW (<50%).\n";
            std::cout << "Consider integrating killers into quiet scoring.\n";
        } else if (hit_rate < 30.0) {
            std::cout << "RECOMMENDATION: Hit rate is LOW (<30%).\n";
            std::cout << "Killers may not be very effective at this position.\n";
        } else {
            std::cout << "ASSESSMENT: Killer moves are working well.\n";
        }

        std::cout << "=============================================\n\n";
    }
};

// Global killer stats (for debugging)
extern KillerStats g_killerStats;

// ============================================================================
// MVV-LVA (Most Valuable Victim - Least Valuable Attacker)
//
// Simple capture ordering: prioritize capturing valuable pieces
// with less valuable attackers.
// ============================================================================

inline int mvv_lva(const Board& board, Move m) {
    Piece victim = board.piece_on(m.to());
    Piece attacker = board.piece_on(m.from());

    if (victim == NO_PIECE) {
        // En passant
        if (m.is_enpassant()) {
            victim = make_piece(~board.side_to_move(), PAWN);
        } else {
            return 0;
        }
    }

    return 10 * PieceValue[type_of(victim)] - PieceValue[type_of(attacker)];
}

// ============================================================================
// Killer Moves
//
// Store quiet moves that caused beta cutoffs. These are likely to be
// good moves in sibling nodes at the same ply.
// ============================================================================

// Cache-aligned Killer Table
class alignas(64) KillerTable {
public:
    static constexpr int MAX_PLY = 128;
    static constexpr int NUM_KILLERS = 2;

    KillerTable() { clear(); }

    void clear() {
        for (int i = 0; i < MAX_PLY; ++i) {
            for (int j = 0; j < NUM_KILLERS; ++j) {
                killers[i][j] = MOVE_NONE;
            }
        }
    }

    void store(int ply, Move m) {
        if (ply >= MAX_PLY) return;

        if (killers[ply][0] == m) return;
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = m;
    }

    bool is_killer(int ply, Move m) const {
        if (ply >= MAX_PLY) return false;
        return killers[ply][0] == m || killers[ply][1] == m;
    }

    Move get(int ply, int slot) const {
        if (ply >= MAX_PLY || slot >= NUM_KILLERS) return MOVE_NONE;
        return killers[ply][slot];
    }

private:
    alignas(64) Move killers[MAX_PLY][NUM_KILLERS];
};

// ============================================================================
// Mate Killers
//
// Stores moves that led to checkmate. These have higher priority than
// regular killer moves because they are proven to be winning.
// ============================================================================

// Cache-aligned Mate Killer Table
class alignas(64) MateKillerTable {
public:
    static constexpr int MAX_PLY = 128;

    MateKillerTable() { clear(); }

    void clear() {
        for (int i = 0; i < MAX_PLY; ++i) {
            mateKiller[i] = MOVE_NONE;
        }
    }

    void store(int ply, Move m) {
        if (ply >= MAX_PLY) return;
        mateKiller[ply] = m;
    }

    bool is_mate_killer(int ply, Move m) const {
        if (ply >= MAX_PLY) return false;
        return mateKiller[ply] == m;
    }

    Move get(int ply) const {
        if (ply >= MAX_PLY) return MOVE_NONE;
        return mateKiller[ply];
    }

private:
    alignas(64) Move mateKiller[MAX_PLY];
};

// ============================================================================
// Counter Move Table
//
// For each piece-to_square combination, store the move that refuted it.
// ============================================================================

// Cache-aligned Counter Move Table
class alignas(64) CounterMoveTable {
public:
    CounterMoveTable() { clear(); }

    void clear() {
        for (int pc = 0; pc < PIECE_NB; ++pc) {
            for (int sq = 0; sq < SQUARE_NB; ++sq) {
                table[pc][sq] = MOVE_NONE;
            }
        }
    }

    void store(Piece pc, Square to, Move counter) {
        table[pc][to] = counter;
    }

    Move get(Piece pc, Square to) const {
        return table[pc][to];
    }

private:
    alignas(64) Move table[PIECE_NB][SQUARE_NB];
};

// ============================================================================
// History Heuristic Table
//
// Track how often each move causes a beta cutoff. Used to order quiet moves.
// Uses butterfly boards: indexed by [color][from_square][to_square]
// ============================================================================

// Cache-aligned History Table (butterfly boards)
class alignas(64) HistoryTable {
public:
    static constexpr int MAX_HISTORY = 16384;

    HistoryTable() { clear(); }

    void clear() {
        for (int c = 0; c < COLOR_NB; ++c) {
            for (int from = 0; from < SQUARE_NB; ++from) {
                for (int to = 0; to < SQUARE_NB; ++to) {
                    table[c][from][to] = 0;
                }
            }
        }
    }

    void update(Color c, Move m, int depth, bool is_cutoff) {
        int bonus = is_cutoff ? depth * depth : -depth * depth;
        update_score(table[c][m.from()][m.to()], bonus);
    }

    void update_with_bonus(Color c, Move m, int bonus) {
        update_score(table[c][m.from()][m.to()], bonus);
    }

    void update_quiet_stats(Color c, Move best, Move* quiets, int quiet_count, int depth) {
        int bonus = std::min(105 + 175 * depth + 11 * depth * depth, 2400);
        update_score(table[c][best.from()][best.to()], bonus);

        int malus = -std::min(80 + 145 * depth + 8 * depth * depth, 1900);
        for (int i = 0; i < quiet_count; ++i) {
            if (quiets[i] != best) {
                update_score(table[c][quiets[i].from()][quiets[i].to()], malus);
            }
        }
    }

    int get(Color c, Move m) const {
        return table[c][m.from()][m.to()];
    }

private:
    alignas(64) int table[COLOR_NB][SQUARE_NB][SQUARE_NB];

    void update_score(int& entry, int bonus) {
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }
};

class ContinuationHistoryEntry;

class alignas(64) ContinuationHistoryEntry {
public:
    static constexpr int MAX_HISTORY = 16384;

    ContinuationHistoryEntry() { clear(); }

    void clear() {
        for (int pt = 0; pt < PIECE_TYPE_NB; ++pt) {
            for (int sq = 0; sq < SQUARE_NB; ++sq) {
                table[pt][sq] = 0;
            }
        }
    }

    int get(PieceType pt, Square to) const {
        return table[pt][to];
    }

    void update(PieceType pt, Square to, int bonus) {
        int& entry = table[pt][to];
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }

    void update_with_depth(PieceType pt, Square to, int depth, bool isCutoff, int weight = 1) {
        int bonus;
        if (isCutoff) {
            bonus = std::min(105 + 175 * depth + 11 * depth * depth, 2400);
        } else {
            bonus = -std::min(80 + 145 * depth + 8 * depth * depth, 1900);
        }
        bonus = bonus * weight;
        int& entry = table[pt][to];
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }

    int& operator()(PieceType pt, Square to) {
        return table[pt][to];
    }

private:
    alignas(64) int table[PIECE_TYPE_NB][SQUARE_NB];
};

class alignas(64) ContinuationHistory {
public:
    ContinuationHistory() { clear(); }

    void clear() {
        for (int pc = 0; pc < PIECE_NB; ++pc) {
            for (int sq = 0; sq < SQUARE_NB; ++sq) {
                table[pc][sq].clear();
            }
        }
    }

    ContinuationHistoryEntry* get_entry(Piece pc, Square to) {
        return &table[pc][to];
    }

    const ContinuationHistoryEntry* get_entry(Piece pc, Square to) const {
        return &table[pc][to];
    }

    int get(Piece prevPc, Square prevTo, PieceType currPt, Square currTo) const {
        return table[prevPc][prevTo].get(currPt, currTo);
    }
    void update(Piece prevPc, Square prevTo, PieceType currPt, Square currTo, int bonus) {
        table[prevPc][prevTo].update(currPt, currTo, bonus);
    }

private:
    alignas(64) ContinuationHistoryEntry table[PIECE_NB][SQUARE_NB];
};

// ============================================================================
// Capture History
//
// Tracks the success of capture moves indexed by [piece][to][captured_piece_type].
// This provides better ordering of captures than pure MVV-LVA by learning
// which captures are actually beneficial in practice.
// ============================================================================

// Cache-aligned Capture History Table
class alignas(64) CaptureHistory {
public:
    static constexpr int MAX_HISTORY = 16384;

    CaptureHistory() { clear(); }

    void clear() {
        for (int pc = 0; pc < PIECE_NB; ++pc) {
            for (int sq = 0; sq < SQUARE_NB; ++sq) {
                for (int cpt = 0; cpt < PIECE_TYPE_NB; ++cpt) {
                    table[pc][sq][cpt] = 0;
                }
            }
        }
    }

    int get(Piece movingPiece, Square to, PieceType capturedType) const {
        return table[movingPiece][to][capturedType];
    }

    void update(Piece movingPiece, Square to, PieceType capturedType, int bonus) {
        int& entry = table[movingPiece][to][capturedType];
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }

    void update_capture_stats(Piece movingPiece, Square to, PieceType capturedType,
                               int depth, bool causedCutoff) {
        int bonus;
        if (causedCutoff) {
            bonus = std::min((105 + 175 * depth + 11 * depth * depth) * SearchParams::CAPTURE_HIST_BONUS_SCALE / 10, 2400);
        } else {
            bonus = -std::min((80 + 145 * depth + 8 * depth * depth) * SearchParams::CAPTURE_HIST_MALUS_SCALE / 10, 1900);
        }
        update(movingPiece, to, capturedType, bonus);
    }

    void update_failed_captures(Piece* pieces, Move* captures, PieceType* capturedTypes,
                                 int count, Move bestCapture, int depth) {
        int malus = -std::min((80 + 145 * depth + 8 * depth * depth) * SearchParams::CAPTURE_HIST_MALUS_SCALE / 10, 1900);
        for (int i = 0; i < count; ++i) {
            if (captures[i] != bestCapture && capturedTypes[i] != NO_PIECE_TYPE) {
                update(pieces[i], captures[i].to(), capturedTypes[i], malus);
            }
        }
    }

private:
    alignas(64) int table[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];
};

// ============================================================================
// Move Ordering Statistics
//
// Tracks statistics about move ordering effectiveness for analysis and tuning.
// Records how often different ordering mechanisms find the best move.
// ============================================================================

class MoveOrderStats {
public:
    MoveOrderStats() { reset(); }

    void reset() {
        ttMoveSuccess = 0;
        ttMoveAttempts = 0;
        killerMoveSuccess = 0;
        killerMoveAttempts = 0;
        counterMoveSuccess = 0;
        counterMoveAttempts = 0;
        historyMoveSuccess = 0;
        historyMoveAttempts = 0;
        captureHistorySuccess = 0;
        captureHistoryAttempts = 0;
        firstMoveSuccess = 0;
        totalNodes = 0;
    }
    void record_tt_cutoff(bool success) {
        ttMoveAttempts++;
        if (success) ttMoveSuccess++;
    }
    void record_killer_cutoff(bool success) {
        killerMoveAttempts++;
        if (success) killerMoveSuccess++;
    }
    void record_counter_cutoff(bool success) {
        counterMoveAttempts++;
        if (success) counterMoveSuccess++;
    }
    void record_history_cutoff(bool success) {
        historyMoveAttempts++;
        if (success) historyMoveSuccess++;
    }
    void record_capture_history(bool success) {
        captureHistoryAttempts++;
        if (success) captureHistorySuccess++;
    }
    void record_first_move_best() {
        firstMoveSuccess++;
    }

    void record_node() {
        totalNodes++;
    }

    double tt_success_rate() const {
        return ttMoveAttempts > 0 ? 100.0 * ttMoveSuccess / ttMoveAttempts : 0.0;
    }
    double killer_success_rate() const {
        return killerMoveAttempts > 0 ? 100.0 * killerMoveSuccess / killerMoveAttempts : 0.0;
    }

    double counter_success_rate() const {
        return counterMoveAttempts > 0 ? 100.0 * counterMoveSuccess / counterMoveAttempts : 0.0;
    }

    double history_success_rate() const {
        return historyMoveAttempts > 0 ? 100.0 * historyMoveSuccess / historyMoveAttempts : 0.0;
    }

    double capture_history_success_rate() const {
        return captureHistoryAttempts > 0 ? 100.0 * captureHistorySuccess / captureHistoryAttempts : 0.0;
    }

    double first_move_rate() const {
        return totalNodes > 0 ? 100.0 * firstMoveSuccess / totalNodes : 0.0;
    }

    U64 get_total_nodes() const { return totalNodes; }
    U64 get_tt_attempts() const { return ttMoveAttempts; }
    U64 get_tt_success() const { return ttMoveSuccess; }
    U64 get_first_move_success() const { return firstMoveSuccess; }

private:
    U64 ttMoveSuccess;
    U64 ttMoveAttempts;
    U64 killerMoveSuccess;
    U64 killerMoveAttempts;
    U64 counterMoveSuccess;
    U64 counterMoveAttempts;
    U64 historyMoveSuccess;
    U64 historyMoveAttempts;
    U64 captureHistorySuccess;
    U64 captureHistoryAttempts;
    U64 firstMoveSuccess;
    U64 totalNodes;
};


// ============================================================================
// Move Picker
//
// Generates and orders moves lazily, returning them one at a time
// in order of expected quality.
// ============================================================================

enum MovePickStage {
    STAGE_TT_MOVE,
    STAGE_GENERATE_CAPTURES,
    STAGE_WINNING_CAPTURES,
    STAGE_GENERATE_QUIET_CHECKS,
    STAGE_QUIET_CHECKS,
    STAGE_KILLER_1,
    STAGE_KILLER_2,
    STAGE_COUNTER_MOVE,
    STAGE_GENERATE_QUIETS,
    STAGE_EQUAL_CAPTURES,
    STAGE_GOOD_QUIETS,         // Quiet moves with score > threshold (searched before bad captures)
    STAGE_BAD_CAPTURES,
    STAGE_BAD_QUIETS,          // Quiet moves with score <= threshold (searched last)
    STAGE_DONE,
    STAGE_QS_TT_MOVE,
    STAGE_QS_GENERATE_CAPTURES,
    STAGE_QS_CAPTURES,
    STAGE_QS_DONE
};

// Good/Bad quiet threshold (Stockfish uses -14000)
// Quiets with score > threshold are considered "good"
constexpr int GOOD_QUIET_THRESHOLD = -14000;

inline MovePickStage& operator++(MovePickStage& s) {
    return s = MovePickStage(int(s) + 1);
}

class MovePicker {
public:
    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, int ply,
               const KillerTable& kt, const MateKillerTable& mkt, const CounterMoveTable& cm,
               const HistoryTable& ht, Move prevMove,
               const ContinuationHistoryEntry* contHist1 = nullptr,
               const ContinuationHistoryEntry* contHist2 = nullptr,
               const ContinuationHistoryEntry* contHist4 = nullptr,
               const CaptureHistory* captHist = nullptr);

    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, const HistoryTable& ht);

    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, const HistoryTable& ht,
               const CaptureHistory* captHist);

    Move next_move();

private:
    const Board& board;
    const HistoryTable& history;
    const KillerTable* killers;
    const MateKillerTable* mateKillers;
    const CounterMoveTable* counterMoves;
    const ContinuationHistoryEntry* contHist1ply;
    const ContinuationHistoryEntry* contHist2ply;
    const ContinuationHistoryEntry* contHist4ply;
    const CaptureHistory* captureHist;

    Move ttMoves[3];
    int ttMoveCount;
    int ttMoveIdx;

    Move killer1, killer2;
    Move mateKiller;
    Move counterMove;

    MoveList moves;
    MoveList badCaptures;
    MoveList equalCaptures;
    MoveList quietChecks;

    static constexpr int MAX_QUIET_CHECKS = 32;
    Move quietCheckMoves[MAX_QUIET_CHECKS];
    int quietCheckCount;

    int currentIdx;
    int equalCaptureIdx;
    int quietCheckIdx;
    int badCaptureIdx;
    int endGoodQuiets;  // Index marking end of good quiets (for good/bad split)
    int ply;

    MovePickStage stage;

    void score_captures();
    void score_quiets();
    void score_quiet_checks();
    Move pick_best();
    bool is_tt_move(Move m) const;
    bool is_quiet_check(Move m) const;
};

#endif