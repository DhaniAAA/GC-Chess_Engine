#ifndef MOVEORDER_HPP
#define MOVEORDER_HPP

#include "board.hpp"
#include "move.hpp"
#include "tt.hpp"

// ============================================================================
// Move Ordering Scores
// Higher scores = searched first
// ============================================================================

constexpr int SCORE_TT_MOVE      = 10000000;  // Hash move from TT
constexpr int SCORE_WINNING_CAP  = 8000000;   // Winning capture (SEE > 0)
constexpr int SCORE_PROMOTION    = 7000000;   // Promotion moves
constexpr int SCORE_EQUAL_CAP    = 6000000;   // Equal capture (SEE == 0)
constexpr int SCORE_KILLER_1     = 5000000;   // First killer move
constexpr int SCORE_KILLER_2     = 4000000;   // Second killer move
constexpr int SCORE_COUNTER      = 3000000;   // Counter move
constexpr int SCORE_LOSING_CAP   = 0;         // Losing capture (SEE < 0), sorted after quiets

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

// ============================================================================
// Continuation History
//
// Tracks how successful a move is relative to the previous move (1-ply ago)
// and the move before that (2-ply ago). This provides context-aware scoring
// that significantly improves move ordering.
//
// Indexed by [PieceType of prev move][ToSquare of prev move][PieceType of curr move][ToSquare of curr move]
// ============================================================================

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
// Move Picker
//
// Generates and orders moves lazily, returning them one at a time
// in order of expected quality.
// ============================================================================

enum MovePickStage {
    STAGE_TT_MOVE,
    STAGE_GENERATE_CAPTURES,
    STAGE_GOOD_CAPTURES,
    STAGE_KILLER_1,
    STAGE_KILLER_2,
    STAGE_COUNTER_MOVE,
    STAGE_GENERATE_QUIETS,
    STAGE_QUIETS,
    STAGE_BAD_CAPTURES,
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
    // Constructor for main search (with continuation history)
    MovePicker(const Board& b, Move ttMove, int ply,
               const KillerTable& kt, const CounterMoveTable& cm,
               const HistoryTable& ht, Move prevMove,
               const ContinuationHistoryEntry* contHist1 = nullptr,
               const ContinuationHistoryEntry* contHist2 = nullptr,
               const int (*captureHistory)[64][8] = nullptr);

    // Constructor for quiescence search
    MovePicker(const Board& b, Move ttMove, const HistoryTable& ht);

    // Get next move (returns MOVE_NONE when exhausted)
    Move next_move();

private:
    const Board& board;
    const HistoryTable& history;
    const KillerTable* killers;
    const CounterMoveTable* counterMoves;
    const ContinuationHistoryEntry* contHist1ply;  // 1-ply ago continuation history
    const ContinuationHistoryEntry* contHist2ply;  // 2-ply ago continuation history
    const int (*captureHistory)[64][8];            // Capture history pointer

    Move ttMove;
    Move killer1, killer2;
    Move counterMove;

    MoveList moves;
    MoveList badCaptures;
    int currentIdx;
    int ply;

    MovePickStage stage;

    void score_captures();
    void score_quiets();
    Move pick_best();
};

#endif // MOVEORDER_HPP
