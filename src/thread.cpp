#include "thread.hpp"
#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include <algorithm>

ThreadPool Threads;

SearchThread::SearchThread(int id)
    : threadId(id), workerSearch(std::make_unique<Search>()) {
    workerSearch->set_thread_id(id);
    nativeThread = std::thread(&SearchThread::idle_loop, this);
}

SearchThread::~SearchThread() {
    exit = true;
    if (workerSearch) {
        workerSearch->stop();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        searching = true;
        cv.notify_one();
    }
    if (nativeThread.joinable()) {
        nativeThread.join();
    }
}

void SearchThread::clear_history() {
    if (workerSearch) {
        workerSearch->clear_history();
    }
}

void SearchThread::start_searching(const Board& rootBoard, const SearchLimits& limits) {
    std::lock_guard<std::mutex> lock(mutex);
    threadBoard.copy_with_history(rootBoard, historyBuf, 512);
    threadLimits = limits;
    searching = true;
    cv.notify_one();
}

void SearchThread::stop() {
    if (workerSearch) {
        workerSearch->stop();
    }
}

void SearchThread::wait_for_search_finished() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return !searching; });
}

void SearchThread::idle_loop() {
    while (!exit) {
        std::unique_lock<std::mutex> lock(mutex);
        searching = false;
        cv.notify_one();
        cv.wait(lock, [this] { return searching || exit; });
        if (exit) break;
        lock.unlock();

        workerSearch->start(threadBoard, threadLimits);
    }
}

ThreadPool::ThreadPool() : numThreads(1) {}

ThreadPool::~ThreadPool() {
    stop();
    wait_for_search_finished();
    helpers.clear();
}

void ThreadPool::set_thread_count(int count) {
    stop();
    wait_for_search_finished();
    helpers.clear();

    numThreads = std::clamp(count, 1, MAX_THREADS);
    for (int i = 1; i < numThreads; ++i) {
        helpers.push_back(std::make_unique<SearchThread>(i));
    }
}

void ThreadPool::start_thinking(Board& board, const SearchLimits& limits) {
    wait_for_search_finished();

    Board mainBoard;
    mainBoard.copy_with_history(board, mainHistoryBuf, 512);

    Searcher.set_thread_id(0);

    // Launch helper threads (if numThreads > 1)
    for (auto& helper : helpers) {
        helper->start_searching(board, limits);
    }

    // Run main search on thread 0
    Searcher.start(mainBoard, limits);

    // Stop helpers as soon as main thread completes
    for (auto& helper : helpers) {
        helper->stop();
    }
    wait_for_search_finished();
}

void ThreadPool::stop() {
    Searcher.stop();
    for (auto& helper : helpers) {
        helper->stop();
    }
}

void ThreadPool::on_ponderhit() {
    Searcher.on_ponderhit();
    for (auto& helper : helpers) {
        // Helpers continue searching with updated main thread time control
        (void)helper;
    }
}

void ThreadPool::wait_for_search_finished() {
    for (auto& helper : helpers) {
        helper->wait_for_search_finished();
    }
}

U64 ThreadPool::total_nodes() const {
    U64 total = Searcher.stats().nodes;
    for (const auto& helper : helpers) {
        total += helper->nodes();
    }
    return total;
}

U64 ThreadPool::total_tb_hits() const {
    U64 total = Searcher.stats().tbHits;
    for (const auto& helper : helpers) {
        total += helper->tb_hits();
    }
    return total;
}

int ThreadPool::max_sel_depth() const {
    int maxSD = Searcher.stats().selDepth;
    for (const auto& helper : helpers) {
        maxSD = std::max(maxSD, helper->sel_depth());
    }
    return maxSD;
}

Move ThreadPool::best_move() const {
    return Searcher.best_move();
}

Move ThreadPool::ponder_move() const {
    return Searcher.ponder_move();
}

int ThreadPool::best_score() const {
    return Searcher.best_score();
}

void ThreadPool::clear_all_history() {
    Searcher.clear_history();
    for (auto& helper : helpers) {
        helper->clear_history();
    }
}
