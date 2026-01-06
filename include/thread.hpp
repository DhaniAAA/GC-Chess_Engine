#ifndef THREAD_HPP
#define THREAD_HPP

#include "search.hpp"  // For SearchLimits, MAX_PLY, MAX_MOVES, score functions
#include "board.hpp"
#include "move.hpp"
#include "moveorder.hpp"
#include "tt.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Forward declaration
class ThreadPool;

// ============================================================================
// Thread Constants
// ============================================================================

constexpr int MAX_THREADS = 256;


// ============================================================================
// Per-Thread Search Stack
// ============================================================================

// Cache-line aligned ThreadStack to prevent false sharing
struct alignas(64) ThreadStack {
    int ply;
    Move currentMove;
    Move excludedMove;
    Move killers[2];
    int staticEval;
    int moveCount;
    int extensions;
    bool inCheck;
    bool ttPv;
    bool ttHit;
    bool nullMovePruned;
    char padding[20];  // Pad to prevent false sharing
};

// ============================================================================
// PV Line for each thread
// ============================================================================

struct ThreadPVLine {
    int length = 0;
    Move moves[MAX_PLY];

    void clear() { length = 0; }

    void update(Move m, const ThreadPVLine& child) {
        moves[0] = m;
        for (int i = 0; i < child.length && i + 1 < MAX_PLY; ++i) {
            moves[i + 1] = child.moves[i];
        }
        length = child.length + 1;
    }

    std::string to_string() const {
        std::string s;
        for (int i = 0; i < length; ++i) {
            if (i > 0) s += " ";
            s += move_to_string(moves[i]);
        }
        return s;
    }

    Move first() const {
        return length > 0 ? moves[0] : MOVE_NONE;
    }

    Move second() const {
        return length > 1 ? moves[1] : MOVE_NONE;
    }
};

// ============================================================================
// Thread Class - Each thread runs its own search
// ============================================================================

// Cache-line aligned SearchThread to prevent false sharing between threads
class alignas(64) SearchThread {
public:
    explicit SearchThread(int id);
    ~SearchThread();

    void start_searching();
    void wait_for_search_finished();

    int id() const { return threadId; }
    bool is_main() const { return threadId == 0; }

    // ========================================================================
    // HOT DATA - Frequently accessed during search (keep in first cache lines)
    // ========================================================================

    alignas(64) std::atomic<bool> searching{false};
    std::atomic<bool> exit{false};

    alignas(64) U64 nodes = 0;
    U64 tbHits = 0;
    int selDepth = 0;
    int completedDepth = 0;
    int bestScore = 0;
    Move bestMove = MOVE_NONE;
    Move ponderMove = MOVE_NONE;

    // ========================================================================
    // WARM DATA - Accessed often but less frequently
    // ========================================================================

    alignas(64) Board* rootBoard = nullptr;
    int rootDepth = 0;
    Move previousMove = MOVE_NONE;
    U64 rand_seed;

    // ========================================================================
    // LARGE DATA STRUCTURES - Cache-aligned for efficiency
    // ========================================================================

    alignas(64) KillerTable killers;
    alignas(64) CounterMoveTable counterMoves;
    alignas(64) HistoryTable history;

    alignas(64) ThreadStack stack[MAX_PLY + 4];
    alignas(64) ThreadPVLine pvLines[MAX_PLY];

    void clear_history();
    int rand_int(int max);

private:
    int threadId;
    std::thread nativeThread;
    std::mutex mutex;
    std::condition_variable cv;

    void idle_loop();
};

// ============================================================================
// Thread Pool - Manages all search threads
// ============================================================================

class ThreadPool {
public:
    ThreadPool();
    ~ThreadPool();

    void set_thread_count(int count);
    int thread_count() const { return static_cast<int>(threads.size()); }

    SearchThread* main() { return threads.empty() ? nullptr : threads[0].get(); }
    const SearchThread* main() const { return threads.empty() ? nullptr : threads[0].get(); }

    void start_thinking(Board& board, const SearchLimits& limits);
    void stop();
    void on_ponderhit();
    void wait_for_search_finished();
    bool searching() const;

    U64 total_nodes() const;
    U64 total_tb_hits() const;
    int max_sel_depth() const;
    Move best_move() const;
    Move ponder_move() const;
    int best_score() const;

    std::atomic<bool> stop_flag{false};
    SearchLimits limits;

    std::chrono::steady_clock::time_point startTime;
    int optimumTime = 0;
    int maximumTime = 0;

    void clear_all_history();

private:
    std::vector<std::unique_ptr<SearchThread>> threads;

    void init_time_management(Color us);
};

// ============================================================================
// Global Thread Pool Instance
// ============================================================================

extern ThreadPool Threads;

// ============================================================================
// Lazy SMP Search Functions (implemented in thread.cpp)
// ============================================================================

namespace LazySMP {

void search(SearchThread* thread, Board& board, int depth);
void iterative_deepening(SearchThread* thread, Board& board);

int alpha_beta(SearchThread* thread, Board& board, int alpha, int beta,
               int depth, bool cutNode, int ply);

int qsearch(SearchThread* thread, Board& board, int alpha, int beta, int ply);

int evaluate(const Board& board);

bool should_stop(SearchThread* thread);

void report_info(SearchThread* thread, int depth, int score);

}

#endif
