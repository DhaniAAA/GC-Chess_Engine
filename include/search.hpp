#ifndef SEARCH_HPP
#define SEARCH_HPP

#include "board.hpp"
#include "move.hpp"
#include "moveorder.hpp"
#include "tt.hpp"
#include <atomic>
#include <chrono>
#include <vector>

// ============================================================================
// Search Constants
// ============================================================================

constexpr int MAX_PLY = 128;
constexpr int MAX_MOVES = 256;

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
    int moveCount;
    int extensions;       // Total extensions in this path (for limiting)
    bool inCheck;
    bool ttPv;
    bool ttHit;
    bool nullMovePruned;  // Was null move tried at this node?
    ContinuationHistoryEntry* contHistory;  // Pointer to continuation history entry for this move
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
    PVLine pv;
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
    void report_info(int depth, int score, const PVLine& pv);

    // Move ordering tables
    KillerTable killers;
    CounterMoveTable counterMoves;
    HistoryTable history;
    ContinuationHistory contHistory;  // Continuation history (1-ply and 2-ply ago tracking)

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
