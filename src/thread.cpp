#include "thread.hpp"
#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include "search_constants.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>

// ============================================================================
// Global Thread Pool Instance
// ============================================================================

ThreadPool Threads;

// ============================================================================
// LMR Table (shared with search.cpp)
// ============================================================================

extern int LMRTable[64][64];

// Using shared search parameters from search_constants.hpp
using namespace SearchParams;

// ============================================================================
// SearchThread Implementation
// ============================================================================

SearchThread::SearchThread(int id) : rand_seed(id + 1), threadId(id) {
    // Initialize search stack
    for (int i = 0; i < MAX_PLY + 4; ++i) {
        stack[i].ply = i - 2;
        stack[i].currentMove = MOVE_NONE;
        stack[i].excludedMove = MOVE_NONE;
        stack[i].killers[0] = MOVE_NONE;
        stack[i].killers[1] = MOVE_NONE;
        stack[i].extensions = 0;
        stack[i].nullMovePruned = false;
    }

    // Start thread in idle loop
    nativeThread = std::thread(&SearchThread::idle_loop, this);
}

SearchThread::~SearchThread() {
    exit = true;
    start_searching();  // Wake up the thread
    if (nativeThread.joinable()) {
        nativeThread.join();
    }
}

void SearchThread::clear_history() {
    killers.clear();
    counterMoves.clear();
    history.clear();
}

int SearchThread::rand_int(int max) {
    // Simple xorshift random
    rand_seed ^= rand_seed << 13;
    rand_seed ^= rand_seed >> 7;
    rand_seed ^= rand_seed << 17;
    return static_cast<int>(rand_seed % max);
}

void SearchThread::start_searching() {
    std::lock_guard<std::mutex> lock(mutex);
    searching = true;
    cv.notify_one();
}

void SearchThread::wait_for_search_finished() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return !searching; });
}

void SearchThread::idle_loop() {
    while (!exit) {
        std::unique_lock<std::mutex> lock(mutex);
        searching = false;
        cv.notify_one();  // Notify waiting threads we're done
        cv.wait(lock, [this] { return searching || exit; });
        lock.unlock();

        if (exit) break;

        // Perform search
        if (rootBoard && !Threads.stop_flag) {
            Board board = *rootBoard;  // Copy board for this thread
            LazySMP::iterative_deepening(this, board);
        }
    }
}

// ============================================================================
// ThreadPool Implementation
// ============================================================================

ThreadPool::ThreadPool() {
    // Create main thread by default
    set_thread_count(1);
}

ThreadPool::~ThreadPool() {
    stop();
    threads.clear();
}

void ThreadPool::set_thread_count(int count) {
    // Stop existing threads
    stop();
    wait_for_search_finished();

    // Clear existing threads
    threads.clear();

    // Create new threads
    count = std::clamp(count, 1, MAX_THREADS);
    for (int i = 0; i < count; ++i) {
        threads.push_back(std::make_unique<SearchThread>(i));
    }
}

void ThreadPool::start_thinking(Board& board, const SearchLimits& lim) {
    wait_for_search_finished();

    limits = lim;
    stop_flag = false;

    init_time_management(board.side_to_move());
    startTime = std::chrono::steady_clock::now();

    // Try opening book first (main thread only, if not in analysis mode)
    if (!limits.infinite && Book::book.is_loaded()) {
        Move bookMove = Book::book.probe(board);
        if (bookMove != MOVE_NONE) {
            main()->bestMove = bookMove;
            main()->completedDepth = 1;
            std::cout << "info string Book move: " << move_to_string(bookMove) << std::endl;
            std::cout << "info depth 1 score cp 0 nodes 0 time 0 pv "
                      << move_to_string(bookMove) << std::endl;
            return;
        }
    }

    // Try tablebase at root
    if (Tablebase::TB.is_initialized() && Tablebase::TB.can_probe(board)) {
        Move tbMove = Tablebase::TB.probe_root(board);
        if (tbMove != MOVE_NONE) {
            main()->bestMove = tbMove;
            main()->completedDepth = 100;
            Tablebase::WDLScore wdl = Tablebase::TB.probe_wdl(board);
            std::cout << "info string Tablebase hit: " << move_to_string(tbMove) << std::endl;
            int score = Tablebase::Tablebases::wdl_to_score(wdl, 0);
            std::cout << "info depth 100 score cp " << score << " nodes 0 time 0 pv "
                      << move_to_string(tbMove) << std::endl;
            return;
        }
    }

    // Prepare all threads
    for (auto& thread : threads) {
        thread->rootBoard = &board;
        thread->rootDepth = 0;
        thread->completedDepth = 0;
        thread->bestMove = MOVE_NONE;
        thread->ponderMove = MOVE_NONE;
        thread->bestScore = 0;
        thread->nodes = 0;
        thread->tbHits = 0;
        thread->selDepth = 0;
    }

    // Start all threads searching
    for (auto& thread : threads) {
        thread->start_searching();
    }
}

void ThreadPool::stop() {
    stop_flag = true;
}

void ThreadPool::on_ponderhit() {
    // Transition from ponder mode to normal search
    // Called when opponent plays the predicted move

    // 1. Disable ponder mode
    limits.ponder = false;

    // 2. Reset start time for time management
    startTime = std::chrono::steady_clock::now();

    // Note: We do NOT stop the search, it continues from the current depth
    // The time checking in should_stop will now start working since limits.ponder is false
}

void ThreadPool::wait_for_search_finished() {
    for (auto& thread : threads) {
        thread->wait_for_search_finished();
    }
}

bool ThreadPool::searching() const {
    for (const auto& thread : threads) {
        if (thread->searching) return true;
    }
    return false;
}

U64 ThreadPool::total_nodes() const {
    U64 total = 0;
    for (const auto& thread : threads) {
        total += thread->nodes;
    }
    return total;
}

U64 ThreadPool::total_tb_hits() const {
    U64 total = 0;
    for (const auto& thread : threads) {
        total += thread->tbHits;
    }
    return total;
}

int ThreadPool::max_sel_depth() const {
    int maxSD = 0;
    for (const auto& thread : threads) {
        maxSD = std::max(maxSD, thread->selDepth);
    }
    return maxSD;
}

Move ThreadPool::best_move() const {
    // Return best move from main thread
    // In a more advanced implementation, we'd pick the best thread
    return main() ? main()->bestMove : MOVE_NONE;
}

Move ThreadPool::ponder_move() const {
    return main() ? main()->ponderMove : MOVE_NONE;
}

int ThreadPool::best_score() const {
    return main() ? main()->bestScore : 0;
}

void ThreadPool::clear_all_history() {
    for (auto& thread : threads) {
        thread->clear_history();
    }
    TT.clear();
}

void ThreadPool::init_time_management(Color us) {
    // Get move overhead from UCI options (default 10ms)
    int moveOverhead = 50;  // Safety buffer for communication lag

    if (limits.movetime > 0) {
        optimumTime = std::max(1, limits.movetime - moveOverhead);
        maximumTime = std::max(1, limits.movetime - moveOverhead);
        return;
    }

    if (limits.time[us] == 0) {
        optimumTime = 1000000;  // Essentially infinite
        maximumTime = 1000000;
        return;
    }

    int time_left = limits.time[us];
    int inc = limits.inc[us];
    int moves_to_go = limits.movestogo > 0 ? limits.movestogo : 30;

    // Reserve time for safety
    int safeTime = std::max(1, time_left - moveOverhead);

    // Base allocation: time_left / moves + increment bonus
    optimumTime = safeTime / moves_to_go + inc * 3 / 4;

    // Maximum time: up to 5x optimal, but never more than 1/3 of remaining time
    maximumTime = std::min(safeTime / 3, optimumTime * 5);

    // Ensure we don't exceed safe time
    optimumTime = std::min(optimumTime, safeTime - 10);
    maximumTime = std::min(maximumTime, safeTime - 10);

    // Minimum time bounds
    optimumTime = std::max(optimumTime, 10);
    maximumTime = std::max(maximumTime, 20);

    // Panic mode: if time is critically low (< 1 second), use minimal time
    if (time_left < 1000) {
        optimumTime = std::min(optimumTime, time_left / 10);
        maximumTime = std::min(maximumTime, time_left / 5);
        optimumTime = std::max(optimumTime, 5);
        maximumTime = std::max(maximumTime, 10);
    }
}

// ============================================================================
// Lazy SMP Search Implementation
// ============================================================================

namespace LazySMP {

bool should_stop(SearchThread* thread) {
    if (Threads.stop_flag) return true;

    if (Threads.limits.infinite || Threads.limits.ponder) return false;

    // Only main thread checks time
    if (!thread->is_main()) return false;

    // Check more frequently when time is low
    // Every 512 nodes normally, every 128 nodes when in panic mode
    int checkInterval = (Threads.limits.time[WHITE] + Threads.limits.time[BLACK] < 10000) ? 127 : 511;
    if ((thread->nodes & checkInterval) != 0) return false;

    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - Threads.startTime).count()
    );

    // Hard limit - must stop immediately
    if (elapsed >= Threads.maximumTime) {
        Threads.stop_flag = true;
        return true;
    }

    // Node limit check
    if (Threads.limits.nodes > 0 && Threads.total_nodes() >= Threads.limits.nodes) {
        Threads.stop_flag = true;
        return true;
    }

    return false;
}

void iterative_deepening(SearchThread* thread, Board& board) {
    int alpha = -VALUE_INFINITE;
    int beta = VALUE_INFINITE;
    int score = 0;

    // Generate root moves
    MoveList rootMoves;
    MoveGen::generate_legal(board, rootMoves);

    if (rootMoves.empty()) return;

    if (rootMoves.size() == 1 && !Threads.limits.infinite && thread->is_main()) {
        thread->bestMove = rootMoves[0].move;
        thread->completedDepth = 1;
        return;
    }

    int maxDepth = Threads.limits.depth > 0 ? Threads.limits.depth : MAX_PLY;

    // Lazy SMP: Non-main threads start at slightly different depths for diversity
    int startDepth = 1;
    if (!thread->is_main()) {
        // Add small random perturbation to depth for diversity
        startDepth = 1 + (thread->id() % 3);
    }

    // Iterative deepening loop
    for (int depth = startDepth; depth <= maxDepth && !Threads.stop_flag; ++depth) {
        thread->rootDepth = depth;

        // Lazy SMP: Skip some depths on helper threads for diversity
        if (!thread->is_main() && depth > 4) {
            // Skip depth with probability based on thread id
            int skipChance = thread->rand_int(4);
            if (skipChance == 0) continue;
        }

        // Aspiration windows
        int delta = 20;

        if (depth >= 5 && thread->completedDepth >= 1) {
            alpha = std::max(score - delta, -VALUE_INFINITE);
            beta = std::min(score + delta, VALUE_INFINITE);
        }

        while (true) {
            int ply = 0;
            thread->pvLines[ply].clear();

            score = alpha_beta(thread, board, alpha, beta, depth, false, ply);

            if (Threads.stop_flag) break;

            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(score - delta, -VALUE_INFINITE);
                delta += delta / 2;
            } else if (score >= beta) {
                beta = std::min(score + delta, VALUE_INFINITE);
                delta += delta / 2;
            } else {
                break;
            }
        }

        if (!Threads.stop_flag) {
            Move bestMoveCandidate = thread->pvLines[0].first();

            // [PERBAIKAN] Validate bestMoveCandidate is legal before storing
            // This catches PV corruption cases that could lead to illegal moves
            // Check both pseudo-legal and legal to catch all edge cases
            if (bestMoveCandidate != MOVE_NONE && thread->rootBoard &&
                (!MoveGen::is_pseudo_legal(*thread->rootBoard, bestMoveCandidate) ||
                 !MoveGen::is_legal(*thread->rootBoard, bestMoveCandidate))) {
                bestMoveCandidate = MOVE_NONE;  // Invalid move, will use fallback
            }

            thread->bestMove = bestMoveCandidate;

            // Validate ponder move by making best move first
            thread->ponderMove = MOVE_NONE;  // Default to no ponder
            if (bestMoveCandidate != MOVE_NONE && thread->pvLines[0].length > 1) {
                Move ponderCandidate = thread->pvLines[0].second();
                if (ponderCandidate != MOVE_NONE && thread->rootBoard) {
                    // Make best move on a copy of the board
                    StateInfo si;
                    Board tempBoard = *thread->rootBoard;
                    tempBoard.do_move(bestMoveCandidate, si);

                    // Check if ponder move is legal in the new position
                    if (MoveGen::is_legal(tempBoard, ponderCandidate)) {
                        thread->ponderMove = ponderCandidate;
                    }
                }
            }

            thread->bestScore = score;
            thread->completedDepth = depth;

            // Only main thread reports info
            if (thread->is_main()) {
                report_info(thread, depth, score);

                // Time management: stop at optimumTime, but continue if early iteration
                auto now = std::chrono::steady_clock::now();
                int elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - Threads.startTime).count()
                );

                // Stop when we've used optimal time
                // Allow early break if we've used most of optimal time and have a stable move
                if (elapsed >= Threads.optimumTime) {
                    break;
                }

                // Early break at 60% of optimal time if best move hasn't changed
                if (depth >= 8 && elapsed >= Threads.optimumTime * 6 / 10) {
                    // Continue searching to use remaining time
                }
            }
        }
    }
}

int alpha_beta(SearchThread* thread, Board& board, int alpha, int beta,
               int depth, bool cutNode, int ply) {
    const bool pvNode = (beta - alpha) > 1;

    // [GUARD PLY] Prevent array overflow and infinite recursion
    if (ply >= MAX_PLY) {
        return evaluate(board);
    }

    // Update selective depth
    if (ply > thread->selDepth) {
        thread->selDepth = ply;
    }

    // Check for stop
    if (should_stop(thread)) return 0;

    // Initialize PV for this ply BEFORE anything else
    // This MUST happen before depth check to prevent stale PV data
    if (ply < MAX_PLY) {
        thread->pvLines[ply].clear();
    }

    // Quiescence at depth 0
    if (depth <= 0) {
        return qsearch(thread, board, alpha, beta, ply);
    }

    ++thread->nodes;

    // Mate distance pruning
    alpha = std::max(alpha, -VALUE_MATE + ply);
    beta = std::min(beta, VALUE_MATE - ply - 1);
    if (alpha >= beta) return alpha;

    // Draw detection - CRITICAL: was missing in thread pool!
    // Check repetition, 50-move rule, and insufficient material
    if (ply > 0 && board.is_draw(ply)) {
        return 0;  // Draw score
    }

    // Also clear child's PV to prevent stale moves from previous searches
    if (ply + 1 < MAX_PLY) {
        thread->pvLines[ply + 1].clear();
    }

    // TT probe
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    // Get multiple moves
    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    // Primary Move
    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;

    // [TT VALIDATION] Validate ttMove is pseudo-legal AND legal to prevent hash collision issues
    // If the move is illegal (could happen with hash collision), ignore the entire TT entry
    if (ttMove != MOVE_NONE && (!MoveGen::is_pseudo_legal(board, ttMove) || !MoveGen::is_legal(board, ttMove))) {
        ttMove = MOVE_NONE;
        ttHit = false;  // Treat as if we didn't find anything
        if (ttMoveCount > 0) ttMoves[0] = MOVE_NONE;
    }

    int ttScore = ttHit ? score_from_tt(tte->score(), ply) : VALUE_NONE;
    int ttDepth = ttHit ? tte->depth() : 0;
    Bound ttBound = ttHit ? tte->bound() : BOUND_NONE;

    // [PERBAIKAN] Validasi tambahan untuk mate scores dari TT
    // Mate score dari hash collision sangat berbahaya - bisa return evaluasi salah
    if (ttHit && ttMove == MOVE_NONE && std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY) {
        ttHit = false;
        ttScore = VALUE_NONE;
    }

    // TT cutoff (non-PV nodes)
    // [PERBAIKAN] Skip TT cutoff untuk mate scores yang tidak masuk akal
    bool ttMateScore = std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY;
    bool allowTTCutoff = !pvNode && ttHit && ttDepth >= depth;

    if (allowTTCutoff && ttMateScore) {
        // [PERBAIKAN KRITIS] Validasi ketat untuk mate scores
        if (ttMove == MOVE_NONE) {
            allowTTCutoff = false;
        } else {
            int mateDistance = std::abs(VALUE_MATE - std::abs(ttScore));
            if (ttDepth < mateDistance * 2) {
                allowTTCutoff = false;
            }
            if (mateDistance < 1 || (ply > 0 && mateDistance > ply + depth)) {
                allowTTCutoff = false;
            }
        }
    }

    if (allowTTCutoff) {
        if ((ttBound == BOUND_EXACT) ||
            (ttBound == BOUND_LOWER && ttScore >= beta) ||
            (ttBound == BOUND_UPPER && ttScore <= alpha)) {
            // [PERBAIKAN] Update PV dengan ttMove
            if (ttMove != MOVE_NONE && ply < MAX_PLY) {
                thread->pvLines[ply].length = 1;
                thread->pvLines[ply].moves[0] = ttMove;
            }
            return ttScore;
        }
    }

    // Static evaluation
    int staticEval;
    bool inCheck = board.in_check();

    if (inCheck) {
        staticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        staticEval = tte->eval();
    } else {
        staticEval = evaluate(board);
    }

    // Razoring - use predicted depth (modern engine pattern)
    if (!pvNode && !inCheck && depth <= 3 && depth >= 1) {
        int predictedDepth = std::max(1, depth - 1);
        int razorMarg = razoring_margin(predictedDepth);
        if (staticEval + razorMarg <= alpha) {
            int razorScore = qsearch(thread, board, alpha - razorMarg,
                                     alpha - razorMarg + 1, ply);
            if (razorScore <= alpha - razorMarg) {
                return razorScore;
            }
        }
    }

    // Reverse futility pruning
    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMarg = rfp_margin(depth, true);  // Dynamic, assume improving
        if (staticEval - rfpMarg >= beta && staticEval < VALUE_MATE_IN_MAX_PLY) {
            return staticEval;
        }
    }

    // Null move pruning
    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    // [GUARD] Safe stack access with bounds checking
    ThreadStack* ss = (ply + 2 < MAX_PLY + 4) ? &thread->stack[ply + 2] : &thread->stack[MAX_PLY + 3];
    bool doubleNull = (ply >= 1 && ply + 1 < MAX_PLY + 4 && thread->stack[ply + 1].nullMovePruned);

    // Mate Threat Detection - set to true if null move shows opponent can mate us
    bool mateThreat = false;

    if (!pvNode && !inCheck && staticEval >= beta && depth >= 3 &&
        hasNonPawnMaterial && !doubleNull) {

        int R = 3 + depth / 4 + std::min(3, (staticEval - beta) / 200);

        StateInfo si;
        board.do_null_move(si);
        ss->nullMovePruned = true;

        int nullScore = -alpha_beta(thread, board, -beta, -beta + 1,
                                     depth - R - 1, !cutNode, ply + 1);

        board.undo_null_move();
        ss->nullMovePruned = false;

        if (Threads.stop_flag) return 0;

        if (nullScore >= beta) {
            if (nullScore >= VALUE_MATE_IN_MAX_PLY) nullScore = beta;
            return nullScore;
        }
        // MATE THREAT EXTENSION DETECTION
        // If null move shows we are getting mated, set flag to extend search
        else if (depth >= MATE_THREAT_EXT_MIN_DEPTH && nullScore <= VALUE_MATED_IN_MAX_PLY) {
            mateThreat = true;
        }
    }

    // IID
    // Use separate variable to not corrupt depth for TT store and extensions
    int searchDepth = depth;
    if (!ttMove && depth >= 6 && (pvNode || cutNode)) {
        alpha_beta(thread, board, alpha, beta, depth - 2, cutNode, ply);
        tte = TT.probe(board.key(), ttHit);
        // ttMoves update logic skipped for IID simplicity, usually ttMove is enough
        ttMove = ttHit ? tte->move() : MOVE_NONE; // Re-probe single move for IID
    }

    // Internal Iterative Reductions (IIR)
    if (!ttMove && depth >= IIR_MIN_DEPTH) {
        if (pvNode) {
            searchDepth -= IIR_PV_REDUCTION;
        } else if (cutNode) {
            searchDepth -= IIR_CUT_REDUCTION;
        } else {
            searchDepth -= IIR_REDUCTION;
        }
    }

    // Move loop
    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

    MovePicker mp(board, ttMoves, ttMoveCount, ply, thread->killers, thread->counterMoves,
                  thread->history, thread->previousMove);
    Move m;

    while ((m = mp.next_move()) != MOVE_NONE) {
        if (m == ss->excludedMove) continue;
        if (!MoveGen::is_legal(board, m)) continue;

        ++moveCount;

        bool isCapture = !board.empty(m.to()) || m.is_enpassant();
        bool isPromotion = m.is_promotion();
        bool givesCheck = MoveGen::gives_check(board, m);

        // LMP
        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && moveCount > lmp_threshold(depth, true)) {
            continue;
        }

        // Futility pruning
        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture &&
            !isPromotion && bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck) {
            if (staticEval + futility_margin(depth, true) <= alpha) continue;
        }

        // SEE pruning
        if (!pvNode && depth <= 4 && isCapture && !SEE::see_ge(board, m, -50 * depth)) {
            continue;
        }

        // Extensions
        int extension = 0;
        // [GUARD] Safe stack access with bounds checking
        int currentExt = (ply >= 2 && ply + 1 < MAX_PLY + 4) ? thread->stack[ply + 1].extensions : 0;

        // Check extension - safe checks (SEE >= 0) ALWAYS extend
        if (givesCheck && currentExt < MAX_EXTENSIONS && SEE::see_ge(board, m, 0)) {
            extension = 1;
        }

        // In-check extension: when WE are in check, extend to find defensive resources
        if (inCheck && currentExt < MAX_EXTENSIONS) {
            extension = std::max(extension, 1);
        }

        // MATE THREAT EXTENSION
        // When null move detected opponent can mate us, extend to find defenses
        if (mateThreat && currentExt < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        // PV EXTENSION
        // Extend first move in PV nodes at sufficient depth
        if (pvNode && moveCount == 1 && depth >= PV_EXT_MIN_DEPTH &&
            currentExt < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        int newDepth = searchDepth - 1 + extension;
        // [GUARD] Safe stack write with bounds checking
        if (ply + 2 < MAX_PLY + 4) {
            thread->stack[ply + 2].extensions = currentExt + extension;
        }

        // LMR
        // Moves that give check MUST NOT be reduced
        int reduction = 0;
        if (depth >= 3 && moveCount > 1 && !isCapture && !isPromotion && !givesCheck) {
            reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];
            if (pvNode) reduction -= 1;
            if (cutNode) reduction += 1;
            if (inCheck) reduction -= 1;
            reduction = std::clamp(reduction, 0, newDepth - 1);
        }

        // Make move
        StateInfo si;
        board.do_move(m, si);

        int score;
        if (moveCount == 1) {
            score = -alpha_beta(thread, board, -beta, -alpha, newDepth, false, ply + 1);
        } else {
            score = -alpha_beta(thread, board, -alpha - 1, -alpha,
                                newDepth - reduction, true, ply + 1);

            if (score > alpha && reduction > 0) {
                score = -alpha_beta(thread, board, -alpha - 1, -alpha,
                                    newDepth, !cutNode, ply + 1);
            }

            if (score > alpha && score < beta) {
                score = -alpha_beta(thread, board, -beta, -alpha, newDepth, false, ply + 1);
            }
        }

        board.undo_move(m);

        if (Threads.stop_flag) return 0;

        // Track quiets
        if (!isCapture && quietCount < 64) {
            quietsSearched[quietCount++] = m;
        }

        // Update best
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;

            if (score > alpha) {
                // [GUARD] Safe PV update with bounds checking
                if (ply + 1 < MAX_PLY) {
                    thread->pvLines[ply].update(m, thread->pvLines[ply + 1]);
                }

                if (score >= beta) {
                    if (!isCapture) {
                        thread->killers.store(ply, m);
                        if (thread->previousMove) {
                            thread->counterMoves.store(board.piece_on(m.from()), m.to(), m);
                        }
                        thread->history.update_quiet_stats(board.side_to_move(), m,
                                                           quietsSearched, quietCount - 1, depth);
                    }
                    break;
                }
                alpha = score;
            }  // close if (ply + 1 < MAX_PLY) - implicitly balanced by scope
        }
    }

    // No legal moves
    if (moveCount == 0) {
        return inCheck ? -VALUE_MATE + ply : 0;
    }

    // Store in TT
    // [PERBAIKAN KRITIS] Jangan simpan ke TT jika search dihentikan
    if (!Threads.stop_flag) {
        Bound bound = bestScore >= beta ? BOUND_LOWER :
                      bestScore > alpha ? BOUND_EXACT : BOUND_UPPER;
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation());
    }

    return bestScore;
}

int qsearch(SearchThread* thread, Board& board, int alpha, int beta, int ply) {
    ++thread->nodes;

    if (should_stop(thread)) return 0;

    // [GUARD PLY] Prevent array overflow at maximum ply
    if (ply >= MAX_PLY) {
        return evaluate(board);
    }

    bool inCheck = board.in_check();
    int staticEval = inCheck ? -VALUE_INFINITE : evaluate(board);

    if (!inCheck) {
        if (staticEval >= beta) return staticEval;
        if (staticEval > alpha) alpha = staticEval;
    }

    MoveList moves;
    if (inCheck) {
        MoveGen::generate_evasions(board, moves);
    } else {
        MoveGen::generate_captures(board, moves);
    }

    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    // Get multiple moves
    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;

    MovePicker mp(board, ttMoves, ttMoveCount, thread->history);
    Move m;
    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;
    int moveCount = 0;

    while ((m = mp.next_move()) != MOVE_NONE) {
        if (!MoveGen::is_legal(board, m)) continue;
        ++moveCount;

        // Delta pruning
        if (!inCheck && !m.is_promotion()) {
            int captureValue = PieceValue[type_of(board.piece_on(m.to()))];
            if (staticEval + captureValue + 200 < alpha) continue;
        }

        // SEE pruning
        // [PERBAIKAN] JANGAN prune capture Queen - pengorbanan ratu sering kunci taktik
        PieceType capturedPt = type_of(board.piece_on(m.to()));
        if (!inCheck && capturedPt != QUEEN && !SEE::see_ge(board, m, 0)) continue;

        StateInfo si;
        board.do_move(m, si);
        int score = -qsearch(thread, board, -beta, -alpha, ply + 1);
        board.undo_move(m);

        if (Threads.stop_flag) return 0;

        if (score > bestScore) {
            bestScore = score;
            if (score > alpha) {
                if (score >= beta) return score;
                alpha = score;
            }
        }
    }

    if (inCheck && moveCount == 0) {
        return -VALUE_MATE + ply;
    }

    return bestScore;
}

int evaluate(const Board& board) {
    if (Tablebase::EndgameRules::is_known_draw(board)) {
        return 0;
    }

    int score = Eval::evaluate(board);

    int scaleFactor = Tablebase::EndgameRules::scale_factor(board);
    if (scaleFactor != 128) {
        score = score * scaleFactor / 128;
    }

    return score;
}

void report_info(SearchThread* thread, int depth, int score) {
    auto now = std::chrono::steady_clock::now();
    U64 elapsed = static_cast<U64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - Threads.startTime).count()
    );

    U64 nodes = Threads.total_nodes();
    U64 nps = elapsed > 0 ? nodes * 1000 / elapsed : nodes;

    std::cout << "info depth " << depth
              << " seldepth " << Threads.max_sel_depth();

    if (std::abs(score) >= VALUE_MATE_IN_MAX_PLY) {
        int mateIn = (score > 0) ?
            (VALUE_MATE - score + 1) / 2 : -(VALUE_MATE + score) / 2;
        std::cout << " score mate " << mateIn;
    } else {
        std::cout << " score cp " << score;
    }

    std::cout << " nodes " << nodes
              << " nps " << nps
              << " time " << elapsed
              << " hashfull " << TT.hashfull();

    // [PERBAIKAN] Validate PV line before output
    // Output only valid moves to prevent "Illegal PV move" warnings from cutechess
    std::cout << " pv";
    if (thread->rootBoard) {
        Board tempBoard = *thread->rootBoard;
        const auto& pvLine = thread->pvLines[0];
        for (int i = 0; i < pvLine.length; ++i) {
            Move m = pvLine.moves[i];
            if (m == MOVE_NONE) break;

            // Validate move is legal in current position
            if (!MoveGen::is_pseudo_legal(tempBoard, m) || !MoveGen::is_legal(tempBoard, m)) {
                break;  // Stop at first illegal move
            }

            std::cout << " " << move_to_string(m);

            // Make the move on temp board to validate next move in sequence
            StateInfo si;
            tempBoard.do_move(m, si);
        }
    }

    std::cout << std::endl;
}

}  // namespace LazySMP
