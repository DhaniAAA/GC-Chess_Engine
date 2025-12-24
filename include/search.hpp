#ifndef SEARCH_HPP
#define SEARCH_HPP

#include "board.hpp"
#include "move.hpp"
#include "moveorder.hpp"
#include "tt.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

// ============================================================================
// Search Constants
// ============================================================================

constexpr int MAX_PLY = 128;
constexpr int MAX_MOVES = 256;
constexpr int MAX_MULTI_PV = 500;  // Maximum number of principal variations

// ============================================================================
// Search Limits
// ============================================================================

struct SearchLimits {
    int depth = 0;           // Maximum depth (0 = unlimited)
    int mate = 0;            // Search for mate in N moves
    U64 nodes = 0;           // Maximum nodes to search
    int movetime = 0;        // Time per move in ms
    int time[2] = {0, 0};    // Time left for each side (ms)
    int inc[2] = {0, 0};     // Increment per move (ms)
    int movestogo = 0;       // Moves until next time control
    bool infinite = false;   // Infinite analysis
    bool ponder = false;     // Pondering mode

    std::vector<Move> searchmoves;  // Restrict search to these moves
};

// ============================================================================
// Search Statistics
// ============================================================================

struct SearchStats {
    U64 nodes = 0;
    U64 tbHits = 0;
    int selDepth = 0;
    int hashfull = 0;

    void reset() {
        nodes = 0;
        tbHits = 0;
        selDepth = 0;
        hashfull = 0;
    }
};

// ============================================================================
// Principal Variation
// ============================================================================

struct PVLine {
    int length = 0;
    Move moves[MAX_PLY];

    // Clear the PV line - also reset a few moves to prevent stale data
    void clear() {
        length = 0;
        // Clear first few moves to prevent stale ponder moves
        for (int i = 0; i < 4 && i < MAX_PLY; ++i) {
            moves[i] = MOVE_NONE;
        }
    }

    void update(Move m, const PVLine& child) {
        moves[0] = m;
        int newLen = 1;
        for (int i = 0; i < child.length && i + 1 < MAX_PLY; ++i) {
            moves[i + 1] = child.moves[i];
            newLen++;
        }
        // Clear any old moves beyond the new length to prevent stale data
        for (int i = newLen; i < length && i < MAX_PLY; ++i) {
            moves[i] = MOVE_NONE;
        }
        length = newLen;
    }

    std::string to_string() const {
        std::string s;
        for (int i = 0; i < length; ++i) {
            if (i > 0) s += " ";
            s += move_to_string(moves[i]);
        }
        return s;
    }
};

// ============================================================================
// Search Stack
// Stack of information for each ply in the search tree
// ============================================================================

struct SearchStack {
    Move* pv;
    int ply;
    Move currentMove;
    Move excludedMove;
    Move killers[2];
    int staticEval;
    int correctedStaticEval;  // Static eval after correction history adjustment
    int moveCount;
    int extensions;       // Total extensions in this path (for limiting)
    bool inCheck;
    bool ttPv;
    bool ttHit;
    bool nullMovePruned;  // Was null move tried at this node?
    ContinuationHistoryEntry* contHistory;  // Pointer to continuation history entry for this move
};

// ============================================================================
// Correction History
// Tracks the average difference between static eval and search result
// to correct systematic bias in evaluation
// ============================================================================

class CorrectionHistory {
public:
    // Size of the correction history table (power of 2 for efficient modulo)
    static constexpr int SIZE = 16384;  // 16K entries

    // Weight constants for exponential moving average
    static constexpr int WEIGHT = 256;   // Weight for new entry
    static constexpr int SCALE = 512;    // Scale divisor

    CorrectionHistory() { clear(); }

    void clear() {
        for (int c = 0; c < COLOR_NB; ++c) {
            for (int i = 0; i < SIZE; ++i) {
                table[c][i] = 0;
            }
        }
    }

    // Get the correction value for a position (indexed by pawn structure key)
    // Returns a correction value in centipawns
    int get(Color c, Key pawnKey) const {
        return table[c][pawnKey & (SIZE - 1)] / SCALE;
    }

    // Update correction history when we know the true search result
    // diff = searchScore - staticEval (positive if eval was too pessimistic)
    void update(Color c, Key pawnKey, int diff, int depth) {
        // Weight based on depth - deeper searches provide more reliable data
        int weight = std::min(depth * depth + 2 * depth - 2, 1024);

        // Clamp diff to prevent extreme values
        diff = std::clamp(diff, -400, 400);

        int idx = pawnKey & (SIZE - 1);

        // Exponential moving average update
        // new_value = old_value * (1 - weight/1024) + diff * weight
        table[c][idx] = (table[c][idx] * (1024 - weight) + diff * SCALE * weight) / 1024;

        // Clamp to prevent runaway values
        table[c][idx] = std::clamp(table[c][idx], -128 * SCALE, 128 * SCALE);
    }

private:
    // Table indexed by [color][pawn_structure_hash]
    // Stores scaled correction values
    int table[COLOR_NB][SIZE];
};

// ============================================================================
// Search Info (for UCI output)
// ============================================================================

struct SearchInfo {
    int depth;
    int selDepth;
    int score;
    bool isMate;
    U64 nodes;
    U64 time;
    U64 nps;
    int hashfull;
    int multiPVIdx;  // MultiPV index (1-based for UCI output)
    PVLine pv;
};

// ============================================================================
// Root Move (for MultiPV support)
// Stores information about each legal move at the root position
// ============================================================================

struct RootMove {
    Move move = MOVE_NONE;
    int score = -VALUE_INFINITE;
    int previousScore = -VALUE_INFINITE;
    int selDepth = 0;
    PVLine pv;

    // Constructor
    RootMove() = default;
    explicit RootMove(Move m) : move(m), score(-VALUE_INFINITE), previousScore(-VALUE_INFINITE), selDepth(0) {
        pv.clear();
    }

    // Comparison operators for sorting (by score, descending)
    bool operator<(const RootMove& other) const {
        return score != other.score ? score > other.score : previousScore > other.previousScore;
    }

    bool operator==(Move m) const {
        return move == m;
    }
};

// ============================================================================
// Search Class
// ============================================================================

class Search {
public:
    Search();

    // Start search with the given position and limits
    void start(Board& board, const SearchLimits& limits);

    // Stop the search
    void stop() { stopped = true; }

    // Check if search is running
    bool is_searching() const { return searching; }

    // Get the best move from last search
    Move best_move() const { return rootBestMove; }

    // Get the ponder move (2nd move in PV)
    Move ponder_move() const { return rootPonderMove; }

    // Get search statistics
    const SearchStats& stats() const { return searchStats; }

    // UCI output callback
    using InfoCallback = void(*)(const SearchInfo&);
    void set_info_callback(InfoCallback cb) { infoCallback = cb; }

    // Clear history tables (between games)
    void clear_history();

    // Static evaluation (public for UCI eval command)
    int evaluate(const Board& board);

private:
    // Iterative deepening loop
    void iterative_deepening(Board& board);

    // Alpha-beta search with Principal Variation Search
    int search(Board& board, int alpha, int beta, int depth, bool cutNode);

    // Quiescence search
    // qsDepth: 0 = normal qsearch, > 0 = also search quiet checks
    int qsearch(Board& board, int alpha, int beta, int qsDepth = 0);


    // Time management
    void init_time_management(Color us);
    void check_time();
    bool should_stop() const;

    // UCI output
    void report_info(Board& board, int depth, int score, const PVLine& pv, int multiPVIdx = 1);

    // Move ordering tables
    KillerTable killers;
    CounterMoveTable counterMoves;
    HistoryTable history;
    ContinuationHistory contHistory;  // Continuation history (1-ply and 2-ply ago tracking)
    CorrectionHistory corrHistory;    // Correction history for static eval bias correction

    // Search state
    std::atomic<bool> stopped;
    std::atomic<bool> searching;
    SearchLimits limits;
    SearchStats searchStats;

    // Root info
    Move rootBestMove;
    Move rootPonderMove;
    int rootDepth;
    int rootPly;  // Starting ply for relative ply calculation

    // MultiPV support
    std::vector<RootMove> rootMoves;  // All legal root moves with their scores and PVs
    int pvIdx;                         // Current PV index being searched (0-based)

    // Time management
    std::chrono::steady_clock::time_point startTime;
    int optimumTime;  // Soft time limit
    int maximumTime;  // Hard time limit

    // Search stack and PV storage
    SearchStack stack[MAX_PLY + 4];
    Move pvTable[MAX_PLY][MAX_PLY];
    PVLine pvLines[MAX_PLY];

    // Previous move (for counter move heuristic)
    Move previousMove;

    // Callbacks
    InfoCallback infoCallback = nullptr;
};

// ============================================================================
// Global Search Instance
// ============================================================================

extern Search Searcher;

#endif // SEARCH_HPP
