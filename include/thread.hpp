#ifndef THREAD_HPP
#define THREAD_HPP

#include "search.hpp"
#include "board.hpp"
#include "move.hpp"
#include "tt.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

constexpr int MAX_THREADS = 256;

class alignas(64) SearchThread {
public:
    explicit SearchThread(int id);
    ~SearchThread();

    void start_searching(const Board& rootBoard, const SearchLimits& limits);
    void stop();
    void wait_for_search_finished();
    void clear_history();

    int id() const { return threadId; }
    bool is_main() const { return threadId == 0; }

    U64 nodes() const { return workerSearch ? workerSearch->stats().nodes : 0; }
    U64 tb_hits() const { return workerSearch ? workerSearch->stats().tbHits : 0; }
    int sel_depth() const { return workerSearch ? workerSearch->stats().selDepth : 0; }
    int completed_depth() const { return workerSearch ? workerSearch->completed_depth() : 0; }
    int best_score() const { return workerSearch ? workerSearch->best_score() : -VALUE_INFINITE; }
    Move best_move() const { return workerSearch ? workerSearch->best_move() : MOVE_NONE; }
    Move ponder_move() const { return workerSearch ? workerSearch->ponder_move() : MOVE_NONE; }

private:
    int threadId;
    std::atomic<bool> searching{false};
    std::atomic<bool> exit{false};

    std::unique_ptr<Search> workerSearch;
    Board threadBoard;
    StateInfo historyBuf[512];
    SearchLimits threadLimits;

    std::thread nativeThread;
    std::mutex mutex;
    std::condition_variable cv;

    void idle_loop();
};

class ThreadPool {
public:
    ThreadPool();
    ~ThreadPool();

    void set_thread_count(int count);
    int thread_count() const { return numThreads; }

    void start_thinking(Board& board, const SearchLimits& limits);
    void stop();
    void on_ponderhit();
    void wait_for_search_finished();

    U64 total_nodes() const;
    U64 total_tb_hits() const;
    int max_sel_depth() const;
    Move best_move() const;
    Move ponder_move() const;
    int best_score() const;

    void clear_all_history();

private:
    int numThreads = 1;
    StateInfo mainHistoryBuf[512];
    std::vector<std::unique_ptr<SearchThread>> helpers;
};

extern ThreadPool Threads;

#endif
