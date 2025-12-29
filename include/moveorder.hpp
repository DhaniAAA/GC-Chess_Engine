#ifndef MOVEORDER_HPP
#define MOVEORDER_HPP

#include "board.hpp"
#include "move.hpp"
#include "tt.hpp"

// ============================================================================
// Move Ordering Scores (using int32_t for efficient range)
// Higher scores = searched first
// ============================================================================

// BASE OFFSET untuk memastikan kategori tidak tumpang tindih
constexpr int32_t SCORE_TT_MOVE        = 400000000;   // Harus SELALU tertinggi
constexpr int32_t SCORE_QUEEN_PROMO    = 300000000;   // Promosi Ratu sangat krusial
constexpr int32_t SCORE_KNIGHT_PROMO   = 290000000;   // Promosi Kuda (taktis)
constexpr int32_t SCORE_ROOK_PROMO     = 280000000;   // Promosi Benteng
constexpr int32_t SCORE_BISHOP_PROMO   = 270000000;   // Promosi Gajah
constexpr int32_t SCORE_WINNING_CAP    = 200000000;   // Winning capture (Base value + MVV/LVA nanti)
constexpr int32_t SCORE_EQUAL_CAP      = 190000000;   // Equal capture base (adjusted by piece type)
constexpr int32_t SCORE_MATE_KILLER    = 15000000;    // Mate killer (higher than regular killer)
constexpr int32_t SCORE_KILLER_1       = 10000000;    // Killer move 1
constexpr int32_t SCORE_KILLER_2       = 9000000;     // Killer move 2
constexpr int32_t SCORE_COUNTER        = 8000000;     // Counter move
constexpr int32_t SCORE_HISTORY_MAX    = 7000000;     // Batas atas History Heuristic (biasanya dinamis)
constexpr int32_t SCORE_LOSING_CAP     = -10000000;   // Losing capture (dicari terakhir)

// Equal Capture Bonus by Piece Type (added to SCORE_EQUAL_CAP)
// Trading more valuable pieces is generally more important
constexpr int32_t EQUAL_CAP_QUEEN_BONUS  = 5000;   // QxQ trading - most critical
constexpr int32_t EQUAL_CAP_ROOK_BONUS   = 3000;   // RxR trading - changes game character
constexpr int32_t EQUAL_CAP_BISHOP_BONUS = 1500;   // BxB trading
constexpr int32_t EQUAL_CAP_KNIGHT_BONUS = 1000;   // NxN trading
constexpr int32_t EQUAL_CAP_PAWN_BONUS   = 0;      // PxP trading - least critical

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
    // Returns the SEE value of a capture move
    static int evaluate(const Board& board, Move m);

    // Returns true if SEE value is >= threshold
    static bool see_ge(const Board& board, Move m, int threshold = 0);

private:
    // Get the least valuable attacker of a square
    static PieceType min_attacker(const Board& board, Color side, Square sq,
                                  Bitboard occupied, Bitboard& attackers);
};

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

    // MVV-LVA: 10 * victim_value - attacker_value
    // Higher value = better capture
    return 10 * PieceValue[type_of(victim)] - PieceValue[type_of(attacker)];
}

// ============================================================================
// Killer Moves
//
// Store quiet moves that caused beta cutoffs. These are likely to be
// good moves in sibling nodes at the same ply.
// ============================================================================

class KillerTable {
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

    // Store a killer move at the given ply
    void store(int ply, Move m) {
        if (ply >= MAX_PLY) return;

        // Don't store the same move twice
        if (killers[ply][0] == m) return;

        // Shift killers
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = m;
    }

    // Check if move is a killer at this ply
    bool is_killer(int ply, Move m) const {
        if (ply >= MAX_PLY) return false;
        return killers[ply][0] == m || killers[ply][1] == m;
    }

    // Get killer move
    Move get(int ply, int slot) const {
        if (ply >= MAX_PLY || slot >= NUM_KILLERS) return MOVE_NONE;
        return killers[ply][slot];
    }

private:
    Move killers[MAX_PLY][NUM_KILLERS];
};

// ============================================================================
// Mate Killers
//
// Stores moves that led to checkmate. These have higher priority than
// regular killer moves because they are proven to be winning.
// ============================================================================

class MateKillerTable {
public:
    static constexpr int MAX_PLY = 128;

    MateKillerTable() { clear(); }

    void clear() {
        for (int i = 0; i < MAX_PLY; ++i) {
            mateKiller[i] = MOVE_NONE;
        }
    }

    // Store a mate killer move at the given ply
    void store(int ply, Move m) {
        if (ply >= MAX_PLY) return;
        mateKiller[ply] = m;
    }

    // Check if move is a mate killer at this ply
    bool is_mate_killer(int ply, Move m) const {
        if (ply >= MAX_PLY) return false;
        return mateKiller[ply] == m;
    }

    // Get mate killer move
    Move get(int ply) const {
        if (ply >= MAX_PLY) return MOVE_NONE;
        return mateKiller[ply];
    }

private:
    Move mateKiller[MAX_PLY];
};

// ============================================================================
// Counter Move Table
//
// For each piece-to_square combination, store the move that refuted it.
// ============================================================================

class CounterMoveTable {
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
    Move table[PIECE_NB][SQUARE_NB];
};

// ============================================================================
// History Heuristic Table
//
// Track how often each move causes a beta cutoff. Used to order quiet moves.
// Uses butterfly boards: indexed by [color][from_square][to_square]
// ============================================================================

class HistoryTable {
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

    // Update history score
    void update(Color c, Move m, int depth, bool is_cutoff) {
        int bonus = is_cutoff ? depth * depth : -depth * depth;
        update_score(table[c][m.from()][m.to()], bonus);
    }

    // Bulk update: bonus for best move, penalty for all other tried moves
    void update_quiet_stats(Color c, Move best, Move* quiets, int quiet_count, int depth) {
        int bonus = depth * depth;

        // Bonus for the best move
        update_score(table[c][best.from()][best.to()], bonus);

        // Penalty for all other quiet moves that were tried
        for (int i = 0; i < quiet_count; ++i) {
            if (quiets[i] != best) {
                update_score(table[c][quiets[i].from()][quiets[i].to()], -bonus);
            }
        }
    }

    // Get history score for a move
    int get(Color c, Move m) const {
        return table[c][m.from()][m.to()];
    }

private:
    int table[COLOR_NB][SQUARE_NB][SQUARE_NB];

    // Update with gravity towards 0 to prevent overflow
    void update_score(int& entry, int bonus) {
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }
};

// Continuation History

// Forward declaration for pointer type
class ContinuationHistoryEntry;

// Single piece-to entry: [PieceType][ToSquare]
class ContinuationHistoryEntry {
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

    // Get score for a move
    int get(PieceType pt, Square to) const {
        return table[pt][to];
    }

    // Update score for a move
    void update(PieceType pt, Square to, int bonus) {
        int& entry = table[pt][to];
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }

    // Access operator for Move
    int& operator()(PieceType pt, Square to) {
        return table[pt][to];
    }

private:
    int table[PIECE_TYPE_NB][SQUARE_NB];
};

// Full Continuation History table: [Piece][ToSquare] -> ContinuationHistoryEntry
class ContinuationHistory {
public:
    ContinuationHistory() { clear(); }

    void clear() {
        for (int pc = 0; pc < PIECE_NB; ++pc) {
            for (int sq = 0; sq < SQUARE_NB; ++sq) {
                table[pc][sq].clear();
            }
        }
    }

    // Get the entry for a previous move (piece type and to square)
    ContinuationHistoryEntry* get_entry(Piece pc, Square to) {
        return &table[pc][to];
    }

    const ContinuationHistoryEntry* get_entry(Piece pc, Square to) const {
        return &table[pc][to];
    }

    // Get score for current move given previous move context
    int get(Piece prevPc, Square prevTo, PieceType currPt, Square currTo) const {
        return table[prevPc][prevTo].get(currPt, currTo);
    }

    // Update score for current move given previous move context
    void update(Piece prevPc, Square prevTo, PieceType currPt, Square currTo, int bonus) {
        table[prevPc][prevTo].update(currPt, currTo, bonus);
    }

private:
    ContinuationHistoryEntry table[PIECE_NB][SQUARE_NB];
};

// ============================================================================
// Capture History
//
// Tracks the success of capture moves indexed by [piece][to][captured_piece_type].
// This provides better ordering of captures than pure MVV-LVA by learning
// which captures are actually beneficial in practice.
// ============================================================================

class CaptureHistory {
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

    // Get capture history score
    int get(Piece movingPiece, Square to, PieceType capturedType) const {
        return table[movingPiece][to][capturedType];
    }

    // Update capture history on successful/failed capture
    void update(Piece movingPiece, Square to, PieceType capturedType, int bonus) {
        int& entry = table[movingPiece][to][capturedType];
        // Use gravity formula to prevent overflow (same as HistoryTable)
        entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
    }

    // Bulk update: bonus for successful capture, penalty for failed captures
    void update_capture_stats(Piece movingPiece, Square to, PieceType capturedType,
                               int depth, bool causedCutoff) {
        int bonus = causedCutoff ? depth * depth : -depth * depth / 2;
        update(movingPiece, to, capturedType, bonus);
    }

private:
    int table[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];
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

    // Record when TT move causes cutoff
    void record_tt_cutoff(bool success) {
        ttMoveAttempts++;
        if (success) ttMoveSuccess++;
    }

    // Record when killer move causes cutoff
    void record_killer_cutoff(bool success) {
        killerMoveAttempts++;
        if (success) killerMoveSuccess++;
    }

    // Record when counter move causes cutoff
    void record_counter_cutoff(bool success) {
        counterMoveAttempts++;
        if (success) counterMoveSuccess++;
    }

    // Record when history-ordered move causes cutoff
    void record_history_cutoff(bool success) {
        historyMoveAttempts++;
        if (success) historyMoveSuccess++;
    }

    // Record capture history effectiveness
    void record_capture_history(bool success) {
        captureHistoryAttempts++;
        if (success) captureHistorySuccess++;
    }

    // Record when first move is the best move
    void record_first_move_best() {
        firstMoveSuccess++;
    }

    void record_node() {
        totalNodes++;
    }

    // Get statistics as percentages
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

    // Get raw counts for detailed analysis
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
    // Main search stages - Stockfish-style ordering for optimal cutoff
    STAGE_TT_MOVE,              // 1. Hash move - highest priority
    STAGE_GENERATE_CAPTURES,    // 2. Generate and score all captures
    STAGE_WINNING_CAPTURES,     // 3. Winning captures only (SEE > 0)
    STAGE_KILLER_1,             // 4. Killer move 1 - BEFORE equal captures!
    STAGE_KILLER_2,             // 5. Killer move 2
    STAGE_COUNTER_MOVE,         // 6. Counter move
    STAGE_GENERATE_QUIETS,      // 7. Generate and score quiet moves
    STAGE_EQUAL_CAPTURES,       // 8. [FIX] Equal captures BEFORE quiets (tactical trades)
    STAGE_QUIETS,               // 9. Quiet moves sorted by history
    STAGE_BAD_CAPTURES,         // 10. Losing captures (SEE < 0) - lowest priority
    STAGE_DONE,

    // Quiescence stages
    STAGE_QS_TT_MOVE,
    STAGE_QS_GENERATE_CAPTURES,
    STAGE_QS_CAPTURES,
    STAGE_QS_DONE
};

inline MovePickStage& operator++(MovePickStage& s) {
    return s = MovePickStage(int(s) + 1);
}

class MovePicker {
public:
    // Constructor for main search (with continuation history and capture history)
    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, int ply,
               const KillerTable& kt, const CounterMoveTable& cm,
               const HistoryTable& ht, Move prevMove,
               const ContinuationHistoryEntry* contHist1 = nullptr,
               const ContinuationHistoryEntry* contHist2 = nullptr,
               const CaptureHistory* captHist = nullptr);

    // Constructor for quiescence search (basic - no capture history)
    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, const HistoryTable& ht);

    // Constructor for quiescence search (advanced - with capture history)
    MovePicker(const Board& b, const Move* ttMoves, int ttMoveCount, const HistoryTable& ht,
               const CaptureHistory* captHist);

    // Get next move (returns MOVE_NONE when exhausted)
    Move next_move();

private:
    const Board& board;
    const HistoryTable& history;
    const KillerTable* killers;
    const CounterMoveTable* counterMoves;
    const ContinuationHistoryEntry* contHist1ply;  // 1-ply ago continuation history
    const ContinuationHistoryEntry* contHist2ply;  // 2-ply ago continuation history
    const CaptureHistory* captureHist;             // Capture history class pointer

    Move ttMoves[3];
    int ttMoveCount;
    int ttMoveIdx;   // Current TT move index

    Move killer1, killer2;
    Move counterMove;

    MoveList moves;
    MoveList badCaptures;
    MoveList equalCaptures;  // Equal captures (SEE == 0) deferred after quiets
    int currentIdx;
    int equalCaptureIdx;     // Index for equal captures iteration
    int ply;

    MovePickStage stage;

    void score_captures();
    void score_quiets();
    Move pick_best();
    bool is_tt_move(Move m) const;
};

#endif // MOVEORDER_HPP
