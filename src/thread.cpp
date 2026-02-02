#include "thread.hpp"
#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include "search_constants.hpp"
#include "optimize.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>

ThreadPool Threads;

extern int LMRTable[64][64];

using namespace SearchParams;

SearchThread::SearchThread(int id) : rand_seed(id + 1), threadId(id) {
    for (int i = 0; i < MAX_PLY + 4; ++i) {
        stack[i].ply = i - 2;
        stack[i].currentMove = MOVE_NONE;
        stack[i].excludedMove = MOVE_NONE;
        stack[i].killers[0] = MOVE_NONE;
        stack[i].killers[1] = MOVE_NONE;
        stack[i].extensions = 0;
        stack[i].nullMovePruned = false;
    }

    nativeThread = std::thread(&SearchThread::idle_loop, this);
}

SearchThread::~SearchThread() {
    exit = true;
    start_searching();
    if (nativeThread.joinable()) {
        nativeThread.join();
    }
}

void SearchThread::clear_history() {
    killers.clear();
    mateKillers.clear();
    counterMoves.clear();
    history.clear();
}

int SearchThread::rand_int(int max) {
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
        cv.notify_one();
        cv.wait(lock, [this] { return searching || exit; });
        lock.unlock();

        if (exit) break;

        if (rootBoard && !Threads.stop_flag) {
            Board board = *rootBoard;
            LazySMP::iterative_deepening(this, board);
        }
    }
}

ThreadPool::ThreadPool() {
    set_thread_count(1);
}

ThreadPool::~ThreadPool() {
    stop();
    threads.clear();
}

void ThreadPool::set_thread_count(int count) {
    stop();
    wait_for_search_finished();

    threads.clear();

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

    for (auto& thread : threads) {
        thread->start_searching();
    }
}

void ThreadPool::stop() {
    stop_flag = true;
}

void ThreadPool::on_ponderhit() {
    limits.ponder = false;
    startTime = std::chrono::steady_clock::now();
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
    int moveOverhead = 50;

    if (limits.movetime > 0) {
        optimumTime = std::max(1, limits.movetime - moveOverhead);
        maximumTime = std::max(1, limits.movetime - moveOverhead);
        return;
    }

    if (limits.time[us] == 0) {
        optimumTime = 1000000;
        maximumTime = 1000000;
        return;
    }

    int time_left = limits.time[us];
    int inc = limits.inc[us];
    int moves_to_go = limits.movestogo > 0 ? limits.movestogo : 30;

    int safeTime = std::max(1, time_left - moveOverhead);

    optimumTime = safeTime / moves_to_go + inc * 3 / 4;

    maximumTime = std::min(safeTime / 3, optimumTime * 5);

    optimumTime = std::min(optimumTime, safeTime - 10);
    maximumTime = std::min(maximumTime, safeTime - 10);
    optimumTime = std::max(optimumTime, 10);
    maximumTime = std::max(maximumTime, 20);

    if (time_left < 1000) {
        optimumTime = std::min(optimumTime, time_left / 10);
        maximumTime = std::min(maximumTime, time_left / 5);
        optimumTime = std::max(optimumTime, 5);
        maximumTime = std::max(maximumTime, 10);
    }
}

namespace LazySMP {

bool should_stop(SearchThread* thread) {
    if (UNLIKELY(Threads.stop_flag)) return true;

    if (UNLIKELY(Threads.limits.infinite || Threads.limits.ponder)) return false;

    if (LIKELY(!thread->is_main())) return false;

    int checkInterval = (Threads.limits.time[WHITE] + Threads.limits.time[BLACK] < 10000) ? 127 : 511;
    if (LIKELY((thread->nodes & checkInterval) != 0)) return false;

    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - Threads.startTime).count()
    );

    if (UNLIKELY(elapsed >= Threads.maximumTime)) {
        Threads.stop_flag = true;
        return true;
    }
    if (UNLIKELY(Threads.limits.nodes > 0 && Threads.total_nodes() >= Threads.limits.nodes)) {
        Threads.stop_flag = true;
        return true;
    }

    return false;
}

void iterative_deepening(SearchThread* thread, Board& board) {
    int alpha = -VALUE_INFINITE;
    int beta = VALUE_INFINITE;
    int score = 0;

    MoveList rootMoves;
    MoveGen::generate_legal(board, rootMoves);

    if (rootMoves.empty()) return;

    if (rootMoves.size() == 1 && !Threads.limits.infinite && thread->is_main()) {
        thread->bestMove = rootMoves[0].move;
        thread->completedDepth = 1;
        return;
    }

    int maxDepth = Threads.limits.depth > 0 ? Threads.limits.depth : MAX_PLY;

    int startDepth = 1;
    if (!thread->is_main()) {
        startDepth = 1 + (thread->id() % 3);
    }

    for (int depth = startDepth; depth <= maxDepth && !Threads.stop_flag; ++depth) {
        thread->rootDepth = depth;

        if (!thread->is_main() && depth > 4) {
            int skipChance = thread->rand_int(4);
            if (skipChance == 0) continue;
        }

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

            if (bestMoveCandidate != MOVE_NONE && thread->rootBoard &&
                (!MoveGen::is_pseudo_legal(*thread->rootBoard, bestMoveCandidate) ||
                 !MoveGen::is_legal(*thread->rootBoard, bestMoveCandidate))) {
                bestMoveCandidate = MOVE_NONE;
            }

            thread->bestMove = bestMoveCandidate;

            thread->ponderMove = MOVE_NONE;
            if (bestMoveCandidate != MOVE_NONE && thread->pvLines[0].length > 1) {
                Move ponderCandidate = thread->pvLines[0].second();
                if (ponderCandidate != MOVE_NONE && thread->rootBoard) {
                    StateInfo si;
                    Board tempBoard = *thread->rootBoard;
                    tempBoard.do_move(bestMoveCandidate, si);

                    if (MoveGen::is_legal(tempBoard, ponderCandidate)) {
                        thread->ponderMove = ponderCandidate;
                    }
                }
            }

            thread->bestScore = score;
            thread->completedDepth = depth;

            if (thread->is_main()) {
                report_info(thread, depth, score);

                auto now = std::chrono::steady_clock::now();
                int elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - Threads.startTime).count()
                );

                if (elapsed >= Threads.optimumTime) {
                    break;
                }

                if (depth >= 8 && elapsed >= Threads.optimumTime * 6 / 10) {
                }
            }
        }
    }
}

int alpha_beta(SearchThread* thread, Board& board, int alpha, int beta,
               int depth, bool cutNode, int ply) {
    const bool pvNode = (beta - alpha) > 1;

    if (ply >= MAX_PLY) {
        return evaluate(board);
    }

    if (ply > thread->selDepth) {
        thread->selDepth = ply;
    }
    if (should_stop(thread)) return 0;

    if (ply < MAX_PLY) {
        thread->pvLines[ply].clear();
    }

    if (depth <= 0) {
        return qsearch(thread, board, alpha, beta, ply);
    }

    ++thread->nodes;

    alpha = std::max(alpha, -VALUE_MATE + ply);
    beta = std::min(beta, VALUE_MATE - ply - 1);
    if (alpha >= beta) return alpha;

    const int alphaOrig = alpha;

    if (ply > 0 && board.is_draw(ply)) {
        return 0;
    }

    if (ply + 1 < MAX_PLY) {
        thread->pvLines[ply + 1].clear();
    }

    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;

    if (ttMove != MOVE_NONE && (!MoveGen::is_pseudo_legal(board, ttMove) || !MoveGen::is_legal(board, ttMove))) {
        ttMove = MOVE_NONE;
        ttHit = false;
        if (ttMoveCount > 0) ttMoves[0] = MOVE_NONE;
    }

    int ttScore = ttHit ? score_from_tt(tte->score(), ply) : VALUE_NONE;
    int ttDepth = ttHit ? tte->depth() : 0;
    Bound ttBound = ttHit ? tte->bound() : BOUND_NONE;

    if (ttHit && ttMove == MOVE_NONE && std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY) {
        ttHit = false;
        ttScore = VALUE_NONE;
    }

    bool ttMateScore = std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY;
    bool allowTTCutoff = !pvNode && ttHit && ttDepth >= depth;

    if (allowTTCutoff && ttMateScore) {
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
            if (ttMove != MOVE_NONE && ply < MAX_PLY) {
                thread->pvLines[ply].length = 1;
                thread->pvLines[ply].moves[0] = ttMove;
            }
            return ttScore;
        }
    }

    int staticEval;
    bool inCheck = board.in_check();

    if (inCheck) {
        staticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        staticEval = tte->eval();
    } else {
        staticEval = evaluate(board);
    }

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

    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMarg = rfp_margin(depth, true);
        if (staticEval - rfpMarg >= beta && staticEval < VALUE_MATE_IN_MAX_PLY) {
            return staticEval;
        }
    }

    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    ThreadStack* ss = (ply + 2 < MAX_PLY + 4) ? &thread->stack[ply + 2] : &thread->stack[MAX_PLY + 3];
    bool doubleNull = (ply >= 1 && ply + 1 < MAX_PLY + 4 && thread->stack[ply + 1].nullMovePruned);

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
        else if (depth >= MATE_THREAT_EXT_MIN_DEPTH && nullScore <= VALUE_MATED_IN_MAX_PLY) {
            mateThreat = true;
        }
    }

    int searchDepth = depth;
    if (!ttMove && depth >= 6 && (pvNode || cutNode)) {
        alpha_beta(thread, board, alpha, beta, depth - 2, cutNode, ply);
        tte = TT.probe(board.key(), ttHit);
        ttMove = ttHit ? tte->move() : MOVE_NONE;
    }

    if (!ttMove && depth >= IIR_MIN_DEPTH) {
        if (pvNode) {
            searchDepth -= 1;
        } else if (cutNode) {
            searchDepth -= 2;
        } else {
            searchDepth -= 1;
        }
    }

    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

    MovePicker mp(board, ttMoves, ttMoveCount, ply, thread->killers, thread->mateKillers, thread->counterMoves,
                  thread->history, thread->previousMove,
                  nullptr, nullptr, nullptr, nullptr);
    Move m;

    while ((m = mp.next_move()) != MOVE_NONE) {
        if (m == ss->excludedMove) continue;
        if (!MoveGen::is_legal(board, m)) continue;

        ++moveCount;

        bool isCapture = !board.empty(m.to()) || m.is_enpassant();
        bool isPromotion = m.is_promotion();
        bool givesCheck = MoveGen::gives_check(board, m);

        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && moveCount > lmp_threshold(depth, true)) {
            continue;
        }

        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture &&
            !isPromotion && bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck) {
            if (staticEval + futility_margin(depth, true) <= alpha) continue;
        }

        if (!pvNode && depth <= 4 && isCapture && !SEE::see_ge(board, m, -50 * depth)) {
            continue;
        }

        int extension = 0;
        int currentExt = (ply >= 2 && ply + 1 < MAX_PLY + 4) ? thread->stack[ply + 1].extensions : 0;

        if (givesCheck && currentExt < MAX_EXTENSIONS && SEE::see_ge(board, m, 0)) {
            extension = 1;
        }

        if (inCheck && currentExt < MAX_EXTENSIONS) {
            extension = std::max(extension, 1);
        }

        if (mateThreat && currentExt < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        if (pvNode && moveCount == 1 && depth >= PV_EXT_MIN_DEPTH &&
            currentExt < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        int newDepth = searchDepth - 1 + extension;
        if (ply + 2 < MAX_PLY + 4) {
            thread->stack[ply + 2].extensions = currentExt + extension;
        }

        int reduction = 0;
        if (depth >= 3 && moveCount > 1 && !isCapture && !isPromotion && !givesCheck) {
            reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];
            if (pvNode) reduction -= 1;
            if (cutNode) reduction += 1;
            if (inCheck) reduction -= 1;
            reduction = std::clamp(reduction, 0, newDepth - 1);
        }
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

        if (!isCapture && quietCount < 64) {
            quietsSearched[quietCount++] = m;
        }

        if (score > bestScore) {
            bestScore = score;
            bestMove = m;

            if (score > alpha) {
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
            }
        }
    }

    if (moveCount == 0) {
        return inCheck ? -VALUE_MATE + ply : 0;
    }

    if (!Threads.stop_flag && std::abs(bestScore) < VALUE_MATE_IN_MAX_PLY) {
        Bound bound = bestScore >= beta ? BOUND_LOWER :
                      bestScore > alphaOrig ? BOUND_EXACT : BOUND_UPPER;
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation());
    }

    return bestScore;
}

int qsearch(SearchThread* thread, Board& board, int alpha, int beta, int ply) {
    ++thread->nodes;

    if (should_stop(thread)) return 0;

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

        if (!inCheck && !m.is_promotion()) {
            int captureValue = PieceValue[type_of(board.piece_on(m.to()))];
            if (staticEval + captureValue + 200 < alpha) continue;
        }

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


    std::cout << " pv";
    if (thread->rootBoard) {
        Board tempBoard = *thread->rootBoard;
        const auto& pvLine = thread->pvLines[0];
        for (int i = 0; i < pvLine.length; ++i) {
            Move m = pvLine.moves[i];
            if (m == MOVE_NONE) break;

            if (!MoveGen::is_pseudo_legal(tempBoard, m) || !MoveGen::is_legal(tempBoard, m)) {
                break;
            }

            std::cout << " " << move_to_string(m);

            StateInfo si;
            tempBoard.do_move(m, si);
        }
    }

    std::cout << std::endl;
}

}