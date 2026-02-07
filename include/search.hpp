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

constexpr int MAX_PLY = 128;
constexpr int MAX_MOVES = 256;
constexpr int MAX_MULTI_PV = 500;

struct SearchLimits {
    int depth = 0;
    int mate = 0;
    U64 nodes = 0;
    int movetime = 0;
    int time[2] = {0, 0};
    int inc[2] = {0, 0};
    int movestogo = 0;
    bool infinite = false;
    bool ponder = false;

    std::vector<Move> searchmoves;
};

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

struct PVLine {
    int length = 0;
    Move moves[MAX_PLY];

    void clear() {
        length = 0;
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

struct SearchStack {
    Move* pv;
    int ply;
    Move currentMove;
    Move excludedMove;
    Move killers[2];
    int staticEval;
    int correctedStaticEval;
    int moveCount;
    int extensions;
    int doubleExtensions;
    int tripleExtensions;
    int fractionalExt;
    bool inCheck;
    bool ttPv;
    bool ttHit;
    bool nullMovePruned;
    bool inLMR;
    int reduction;
    int cutoffCnt;
    ContinuationHistoryEntry* contHistory;
};

class CorrectionHistory {
public:
    static constexpr int SIZE = 16384;
    static constexpr int WEIGHT = 256;
    static constexpr int SCALE = 512;

    CorrectionHistory() { clear(); }

    void clear() {
        for (int c = 0; c < COLOR_NB; ++c) {
            for (int i = 0; i < SIZE; ++i) {
                table[c][i] = 0;
            }
        }
    }

    int get(Color c, Key pawnKey) const {
        return table[c][pawnKey & (SIZE - 1)] / SCALE;
    }

    void update(Color c, Key pawnKey, int diff, int depth) {
        int weight = std::min(depth * depth + 2 * depth - 2, 1024);
        diff = std::clamp(diff, -400, 400);

        int idx = pawnKey & (SIZE - 1);

        table[c][idx] = (table[c][idx] * (1024 - weight) + diff * SCALE * weight) / 1024;

        table[c][idx] = std::clamp(table[c][idx], -128 * SCALE, 128 * SCALE);
    }

private:
    int table[COLOR_NB][SIZE];
};

struct SearchInfo {
    int depth;
    int selDepth;
    int score;
    bool isMate;
    U64 nodes;
    U64 time;
    U64 nps;
    int hashfull;
    int multiPVIdx;
    PVLine pv;
};

struct RootMove {
    Move move = MOVE_NONE;
    int score = -VALUE_INFINITE;
    int previousScore = -VALUE_INFINITE;
    int selDepth = 0;
    U64 subtreeNodes = 0;
    U64 prevSubtreeNodes = 0;
    PVLine pv;

    RootMove() = default;
    explicit RootMove(Move m) : move(m), score(-VALUE_INFINITE), previousScore(-VALUE_INFINITE),
                                 selDepth(0), subtreeNodes(0), prevSubtreeNodes(0) {
        pv.clear();
    }

    bool operator<(const RootMove& other) const {
        if (score != other.score) return score > other.score;
        if (previousScore != other.previousScore) return previousScore > other.previousScore;
        return prevSubtreeNodes > other.prevSubtreeNodes;
    }

    bool operator==(Move m) const {
        return move == m;
    }
};

class Search {
public:
    Search();

    void start(Board& board, const SearchLimits& limits);

    void stop() { stopped = true; }

    void on_ponderhit();

    bool is_pondering() const { return isPondering; }

    void set_pondering(bool p) { isPondering = p; }

    bool is_searching() const { return searching; }

    Move best_move() const { return rootBestMove; }

    Move ponder_move() const { return rootPonderMove; }

    const SearchStats& stats() const { return searchStats; }

    using InfoCallback = void(*)(const SearchInfo&);
    void set_info_callback(InfoCallback cb) { infoCallback = cb; }

    void set_silent(bool silent) { silentMode = silent; }
    bool is_silent() const { return silentMode; }

    void clear_history();

    int evaluate(const Board& board);
    int evaluate(const Board& board, int alpha, int beta);

    int qsearch_score(Board& board);

private:
    void iterative_deepening(Board& board);

    int search(Board& board, int alpha, int beta, int depth, bool cutNode);

    int qsearch(Board& board, int alpha, int beta, int qsDepth = 0, Square recaptureSquare = SQ_NONE);

    void init_time_management(Color us);
    void check_time();
    bool should_stop() const;

    void report_info(Board& board, int depth, int score, const PVLine& pv, int multiPVIdx = 1);

    KillerTable killers;
    MateKillerTable mateKillers;
    CounterMoveTable counterMoves;
    HistoryTable history;
    ContinuationHistory contHistory;
    CorrectionHistory corrHistory;
    CaptureHistory captureHist;
    MoveOrderStats moveOrderStats;

    std::atomic<bool> stopped;
    std::atomic<bool> searching;
    std::atomic<bool> isPondering;
    bool silentMode = false;
    SearchLimits limits;
    SearchStats searchStats;

    Move rootBestMove;
    Move rootPonderMove;
    Move previousRootBestMove;
    int previousRootScore;
    int rootDepth;
    int rootPly;
    std::vector<RootMove> rootMoves;
    int pvIdx;

    std::chrono::steady_clock::time_point startTime;
    int optimumTime;
    int maximumTime;

    int bestMoveStability;
    int failLowCount;
    int lastFailLowScore;
    bool emergencyMode;
    int positionComplexity;

    Move previousPV[4];
    int scoreHistory[6];
    int scoreHistoryIdx;
    int pvStability;
    int lastBigScoreChange;

    SearchStack stack[MAX_PLY + 4];
    Move pvTable[MAX_PLY][MAX_PLY];
    PVLine pvLines[MAX_PLY];

    Move previousMove;

    InfoCallback infoCallback = nullptr;
};

extern Search Searcher;

#endif // SEARCH_HPP
