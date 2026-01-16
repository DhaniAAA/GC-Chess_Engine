#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include "uci.hpp"
#include "moveorder.hpp"
#include "search_constants.hpp"
#include "profiler.hpp"
#include "optimize.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <xmmintrin.h>

// ============================================================================
// Global Search Instance
// ============================================================================

Search Searcher;

// ============================================================================
// Reduction Tables
// ============================================================================

int LMRTable[64][64];

void init_lmr_table() {
    for (int depth = 0; depth < 64; ++depth) {
        for (int move = 0; move < 64; ++move) {
            if (depth < 2 || move == 0) {
                LMRTable[depth][move] = 0;
                continue;
            }

            double reduction = SearchParams::LMR_BASE + (std::log(depth) * std::log(move)) / SearchParams::LMR_DIVISOR;
            LMRTable[depth][move] = static_cast<int>(reduction);
        }
    }
}

// ============================================================================
// Using Search Parameters from search_constants.hpp
// ============================================================================

using namespace SearchParams;

// Contempt Factor
int get_contempt(const Board& board) {
    int contempt = UCI::options.contempt;

    if (UCI::options.dynamicContempt) {
        int materialBalance = Eval::material_balance(board);
        Color us = board.side_to_move();

        if (us == WHITE) {
            contempt += materialBalance / 20;
        } else {
            contempt -= materialBalance / 20;
        }

        contempt = std::clamp(contempt, -100, 100);
    }

    return contempt;
}

// ============================================================================
// Search Constructor
// ============================================================================

Search::Search() : stopped(false), searching(false), isPondering(false), rootBestMove(MOVE_NONE),
                   rootPonderMove(MOVE_NONE), previousRootBestMove(MOVE_NONE), previousRootScore(VALUE_NONE),
                   rootDepth(0), rootPly(0), pvIdx(0),
                   optimumTime(0), maximumTime(0), previousMove(MOVE_NONE) {
    static bool lmr_initialized = false;
    if (!lmr_initialized) {
        init_lmr_table();
        lmr_initialized = true;
    }

    for (int i = 0; i < MAX_PLY + 4; ++i) {
        stack[i].ply = i - 2;
        stack[i].currentMove = MOVE_NONE;
        stack[i].excludedMove = MOVE_NONE;
        stack[i].killers[0] = MOVE_NONE;
        stack[i].killers[1] = MOVE_NONE;
        stack[i].extensions = 0;
        stack[i].doubleExtensions = 0;
        stack[i].nullMovePruned = false;
        stack[i].contHistory = nullptr;
    }
}

void Search::clear_history() {
    Eval::pawnTable.clear();
    killers.clear();
    mateKillers.clear();
    counterMoves.clear();
    history.clear();
    contHistory.clear();
    corrHistory.clear();
    captureHist.clear();
    moveOrderStats.reset();
}

// ============================================================================
// Start Search
// ============================================================================

void Search::start(Board& board, const SearchLimits& lim) {
    limits = lim;
    stopped = false;
    searching = true;
    searchStats.reset();

    TT.new_search();

    init_time_management(board.side_to_move());
    startTime = std::chrono::steady_clock::now();

    if (!limits.infinite && Book::book.is_loaded()) {
        Move bookMove = Book::book.probe(board);
        if (bookMove != MOVE_NONE) {
            rootBestMove = bookMove;
            std::cout << "info string Book move: " << move_to_string(bookMove) << std::endl;
            std::cout << "info depth 1 score cp 0 nodes 0 time 0 pv "
                      << move_to_string(bookMove) << std::endl;
            searching = false;
            return;
        }
    }

    if (Tablebase::TB.is_initialized() && Tablebase::TB.can_probe(board)) {
        Move tbMove = Tablebase::TB.probe_root(board);
        if (tbMove != MOVE_NONE) {
            rootBestMove = tbMove;
            Tablebase::WDLScore wdl = Tablebase::TB.probe_wdl(board);
            std::cout << "info string Tablebase hit: " << move_to_string(tbMove) << std::endl;
            int score = Tablebase::Tablebases::wdl_to_score(wdl, 0);
            std::cout << "info depth 100 score cp " << score << " nodes 0 time 0 pv "
                      << move_to_string(tbMove) << std::endl;
            searching = false;
            return;
        }
    }

    iterative_deepening(board);

    searching = false;
    isPondering = false;
}

// ============================================================================
// Ponderhit - Transition from ponder to normal search
// ============================================================================

void Search::on_ponderhit() {
    isPondering = false;
    limits.ponder = false;
    startTime = std::chrono::steady_clock::now();
}

// ============================================================================
// Time Management
// ============================================================================

void Search::init_time_management(Color us) {
    int moveOverhead = 50;

    bestMoveStability = 0;
    failLowCount = 0;
    lastFailLowScore = VALUE_NONE;
    emergencyMode = false;
    positionComplexity = 50;

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

    if (time_left < 500) {

        emergencyMode = true;
        optimumTime = std::max(5, time_left / 20);
        maximumTime = std::max(10, time_left / 10);
        return;
    } else if (time_left < 2000) {
        emergencyMode = true;
        optimumTime = std::max(20, time_left / 15 + inc / 2);
        maximumTime = std::max(50, time_left / 8 + inc / 2);
        return;
    } else if (time_left < 5000) {
        emergencyMode = false;
        optimumTime = time_left / 12 + inc * 2 / 3;
        maximumTime = time_left / 6 + inc;

        optimumTime = std::min(optimumTime, safeTime - 100);
        maximumTime = std::min(maximumTime, safeTime - 50);

        optimumTime = std::max(optimumTime, 30);
        maximumTime = std::max(maximumTime, 60);
        return;
    }

    optimumTime = safeTime / moves_to_go + inc * 3 / 4;
    maximumTime = std::min(safeTime / 3, optimumTime * 5);

    optimumTime = std::min(optimumTime, safeTime - 100);
    maximumTime = std::min(maximumTime, safeTime - 50);

    optimumTime = std::max(optimumTime, 50);
    maximumTime = std::max(maximumTime, 100);
}

void Search::check_time() {
    if (stopped) return;

    if (limits.infinite || limits.ponder || isPondering) return;

    if (limits.depth > 0 && maximumTime == 0 && optimumTime == 0) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed >= maximumTime) {
        stopped = true;
        return;
    }

    if (elapsed >= optimumTime) {
        bool unstable = false;

        if (!rootMoves.empty()) {
            if (previousRootBestMove != MOVE_NONE && rootBestMove != previousRootBestMove) {
                unstable = true;
            }

            if (previousRootScore != VALUE_NONE) {
                int currentPvScore = rootMoves[0].score;

                bool currentIsMate = std::abs(currentPvScore) >= VALUE_MATE_IN_MAX_PLY;
                bool previousIsMate = std::abs(previousRootScore) >= VALUE_MATE_IN_MAX_PLY;

                if (currentIsMate && previousIsMate) {
                    bool sameDirection = (currentPvScore > 0) == (previousRootScore > 0);
                    if (!sameDirection) {
                        unstable = true;
                    }
                } else if (!currentIsMate && !previousIsMate) {
                    if (std::abs(currentPvScore - previousRootScore) > 40) {
                        unstable = true;
                    }
                } else {
                    unstable = true;
                }
            }
        }

        if (!unstable) {
            stopped = true;
        }
    }

    if (limits.nodes > 0 && searchStats.nodes >= limits.nodes) {
        stopped = true;
    }
}

bool Search::should_stop() const {
    return stopped;
}

// ============================================================================
// Iterative Deepening (with MultiPV support)
// ============================================================================

void Search::iterative_deepening(Board& board) {
    rootBestMove = MOVE_NONE;
    rootPonderMove = MOVE_NONE;
    rootPly = board.game_ply();
    pvIdx = 0;

    Board rootBoard = board;
    rootMoves.clear();
    MoveList legalMoves;
    MoveGen::generate_legal(board, legalMoves);

    if (legalMoves.empty()) {
        return;
    }

    for (size_t i = 0; i < legalMoves.size(); ++i) {
        rootMoves.push_back(RootMove(legalMoves[i].move));
    }
    rootBestMove = rootMoves[0].move;

    if (rootMoves.size() == 1 && !limits.infinite) {
        pvIdx = 0;
        rootDepth = 1;

        int score = search(board, -VALUE_INFINITE, VALUE_INFINITE, 1, false);

        if (!stopped) {
            rootMoves[0].score = score;

            if (rootMoves[0].pv.length == 0) {
                rootMoves[0].pv.moves[0] = rootMoves[0].move;
                rootMoves[0].pv.length = 1;
            }
            report_info(rootBoard, 1, score, rootMoves[0].pv, 1);

            std::cout << "info string Only one legal move" << std::endl;
        }

        return;
    }

    int maxDepth = limits.depth > 0 ? limits.depth : MAX_PLY;

    int multiPV = std::min(UCI::options.multiPV, static_cast<int>(rootMoves.size()));
    multiPV = std::max(1, std::min(multiPV, MAX_MULTI_PV));

    previousRootScore = VALUE_NONE;
    previousRootBestMove = MOVE_NONE;

    for (int i = 0; i < 4; ++i) previousPV[i] = MOVE_NONE;
    for (int i = 0; i < 6; ++i) scoreHistory[i] = VALUE_NONE;
    scoreHistoryIdx = 0;
    pvStability = 0;
    lastBigScoreChange = 0;

    std::unordered_map<uint16_t, std::pair<PVLine, int>> bestKnownPVPerMove;
    Move overallBestMove = MOVE_NONE;
    PVLine overallBestPV;
    int overallBestScore = -VALUE_INFINITE;
    overallBestPV.clear();

    for (rootDepth = 1; rootDepth <= maxDepth && !stopped; ++rootDepth) {
        for (auto& rm : rootMoves) {
            rm.previousScore = rm.score;
            rm.prevSubtreeNodes = rm.subtreeNodes;
            rm.subtreeNodes = 0;
        }

        std::vector<RootMove> rootMovesBackup = rootMoves;

        for (pvIdx = 0; pvIdx < multiPV && !stopped; ++pvIdx) {
            Move analyzedMove = rootMoves[pvIdx].move;
            PVLine previousPVLine = rootMoves[pvIdx].pv;
            int previousIterScore = rootMoves[pvIdx].score;

            int alpha = -VALUE_INFINITE;
            int beta = VALUE_INFINITE;
            int delta = ASPIRATION_INITIAL_DELTA;
            int score = rootMoves[pvIdx].previousScore;

            if (rootDepth >= ASPIRATION_MIN_DEPTH && score != -VALUE_INFINITE) {
                alpha = std::max(score - delta, -VALUE_INFINITE);
                beta = std::min(score + delta, VALUE_INFINITE);
            }

            if (pvIdx > 0) {
                int prevScore = rootMoves[pvIdx - 1].score;
                beta = std::min(beta, prevScore + 1);
                alpha = std::min(alpha, prevScore - delta);
            }

            while (true) {
                score = search(board, alpha, beta, rootDepth, false);

                if (stopped) break;

                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

                if (score <= alpha) {
                    beta = (alpha + beta) / 2;
                    delta *= 3;

                    if (delta > 500) {
                        alpha = -VALUE_INFINITE;
                    } else {
                        alpha = std::max(score - delta, -VALUE_INFINITE);
                    }
                } else if (score >= beta) {
                    delta *= 3;

                    if (delta > 500) {
                        beta = VALUE_INFINITE;
                    } else {
                        beta = std::min(score + delta, VALUE_INFINITE);
                    }
                } else {
                    break;
                }
            }

            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

            if (!stopped && analyzedMove != MOVE_NONE) {
                for (auto& rm : rootMoves) {
                    if (rm.move == analyzedMove) {
                        bool suspiciousResult = (rm.pv.length == 0) &&
                                                (previousIterScore != -VALUE_INFINITE) &&
                                                (std::abs(rm.score - previousIterScore) > 500);

                        bool pvDisappeared = (rm.pv.length == 0) && (previousPVLine.length > 0);

                        if (suspiciousResult || pvDisappeared) {
                            rm.pv = previousPVLine;
                            rm.score = previousIterScore;
                        }
                        break;
                    }
                }
            }

            if (!stopped) {
                RootMove& rm = rootMoves[pvIdx];

                bool pvValid = (rm.pv.length > 0) &&
                               (rm.pv.moves[0] != MOVE_NONE) &&
                               MoveGen::is_pseudo_legal(rootBoard, rm.pv.moves[0]) &&
                               MoveGen::is_legal(rootBoard, rm.pv.moves[0]);

                if (pvValid && rm.pv.length > 1) {
                    Move firstMove = rm.pv.moves[0];
                    Move secondMove = rm.pv.moves[1];

                    if (secondMove == MOVE_NONE) {
                        pvValid = false;
                    } else {
                        Board tempBoard = rootBoard;
                        StateInfo si;
                        tempBoard.do_move(firstMove, si);

                        if (!MoveGen::is_pseudo_legal(tempBoard, secondMove) ||
                            !MoveGen::is_legal(tempBoard, secondMove)) {
                            pvValid = false;
                        }
                    }
                }

                if (pvValid) {
                    uint16_t moveKey = rm.move.raw();
                    bestKnownPVPerMove[moveKey] = {rm.pv, rm.score};

                    if (rm.score > overallBestScore || overallBestMove == MOVE_NONE) {
                        overallBestMove = rm.move;
                        overallBestPV = rm.pv;
                        overallBestScore = rm.score;
                    }

                    report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                }
                else {
                    uint16_t moveKey = rm.move.raw();
                    auto it = bestKnownPVPerMove.find(moveKey);

                    if (it != bestKnownPVPerMove.end() && it->second.first.length > 0) {
                        rm.pv = it->second.first;
                        rm.score = it->second.second;
                        report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                    }
                    else if (overallBestPV.length > 0 && overallBestPV.moves[0] != MOVE_NONE) {
                        report_info(rootBoard, rootDepth, overallBestScore, overallBestPV, pvIdx + 1);
                    }
                    else {
                        report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                    }
                }
            }
        }

        if (!stopped && !rootMoves.empty()) {
            bool searchReliable = (rootMoves[0].pv.length > 0) &&
                                  (rootMoves[0].pv.moves[0] != MOVE_NONE) &&
                                  MoveGen::is_pseudo_legal(board, rootMoves[0].pv.moves[0]) &&
                                  MoveGen::is_legal(board, rootMoves[0].pv.moves[0]);

            if (!searchReliable) {
                if (rootMovesBackup.size() > 0 && rootMovesBackup[0].pv.length > 0 &&
                    rootMovesBackup[0].pv.moves[0] != MOVE_NONE) {
                    rootMoves = rootMovesBackup;
                }
                else if (overallBestMove != MOVE_NONE) {
                    for (auto& rm : rootMoves) {
                        uint16_t moveKey = rm.move.raw();
                        auto it = bestKnownPVPerMove.find(moveKey);
                        if (it != bestKnownPVPerMove.end() && it->second.first.length > 0) {
                            rm.pv = it->second.first;
                            rm.score = it->second.second;
                        }
                    }
                }
            }

            const RootMove& bestRM = rootMoves[0];

            if (bestRM.pv.length > 0 && bestRM.pv.moves[0] != MOVE_NONE) {
                Move candidate = bestRM.pv.moves[0];
                if (MoveGen::is_pseudo_legal(board, candidate) && MoveGen::is_legal(board, candidate)) {
                    rootBestMove = candidate;
                }
            } else if (bestRM.move != MOVE_NONE &&
                       MoveGen::is_pseudo_legal(board, bestRM.move) &&
                       MoveGen::is_legal(board, bestRM.move)) {
                rootBestMove = bestRM.move;
            }
            else if (overallBestMove != MOVE_NONE &&
                     MoveGen::is_pseudo_legal(board, overallBestMove) &&
                     MoveGen::is_legal(board, overallBestMove)) {
                rootBestMove = overallBestMove;
            }

            rootPonderMove = MOVE_NONE;
            if (bestRM.pv.length > 1 && rootBestMove != MOVE_NONE) {
                Move ponderCandidate = bestRM.pv.moves[1];
                if (ponderCandidate != MOVE_NONE) {
                    StateInfo si;
                    Board tempBoard = board;
                    tempBoard.do_move(rootBestMove, si);
                    if (MoveGen::is_legal(tempBoard, ponderCandidate)) {
                        rootPonderMove = ponderCandidate;
                    }
                }
            }

            pvLines[0] = bestRM.pv;

            // =========================================================================
            // ADVANCED TIME MANAGEMENT
            // =========================================================================
            if (!limits.infinite && limits.movetime == 0 && !emergencyMode) {
                int score = bestRM.score;

                if (rootDepth >= 4) {
                    if (previousRootBestMove != MOVE_NONE) {
                        if (rootBestMove == previousRootBestMove) {
                            bestMoveStability = std::min(bestMoveStability + 1, 10);
                        } else {
                            bestMoveStability = 0;
                        }
                    }

                    bool failingLow = false;
                    if (previousRootScore != VALUE_NONE) {
                        int scoreDrop = previousRootScore - score;

                        if (scoreDrop > 30) {
                            failLowCount++;
                            failingLow = true;
                            lastFailLowScore = score;
                            int extension = std::min(scoreDrop * 2, optimumTime / 2);
                            optimumTime = std::min(maximumTime, optimumTime + extension);
                        } else if (scoreDrop < -20) {
                            failLowCount = 0;
                        }

                        if (failLowCount >= 2) {
                            int panicExtension = optimumTime / 3;
                            optimumTime = std::min(maximumTime, optimumTime + panicExtension);
                        }
                    }

                    {
                        int complexity = 50;

                        int numMoves = static_cast<int>(rootMoves.size());
                        if (numMoves > 35) complexity += 15;
                        else if (numMoves > 25) complexity += 10;
                        else if (numMoves < 10) complexity -= 15;
                        else if (numMoves < 15) complexity -= 10;

                        if (rootMoves.size() >= 3) {
                            int topScore = rootMoves[0].score;
                            int thirdScore = rootMoves[2].score;
                            int spread = topScore - thirdScore;

                            if (spread < 20) complexity += 20;
                            else if (spread < 50) complexity += 10;
                            else if (spread > 200) complexity -= 15;
                        }
                        if (std::abs(score) > 300) complexity -= 10;
                        if (std::abs(score) > 600) complexity -= 10;

                        complexity -= bestMoveStability * 3;

                        positionComplexity = std::clamp(complexity, 10, 100);

                        double complexityFactor = 0.5 + (positionComplexity / 100.0);
                        int adjustedOptimum = static_cast<int>(optimumTime * complexityFactor);
                        optimumTime = std::min(maximumTime, adjustedOptimum);
                    }

                    auto now = std::chrono::steady_clock::now();
                    int elapsed = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                    );

                    // =========================================================================
                    // SIGNAL 1: Update Score History (for fluctuation detection)
                    // =========================================================================
                    scoreHistory[scoreHistoryIdx] = score;
                    scoreHistoryIdx = (scoreHistoryIdx + 1) % 6;

                    if (previousRootScore != VALUE_NONE) {
                        int scoreDelta = std::abs(score - previousRootScore);
                        if (scoreDelta > 50) {  // More than 50cp change
                            lastBigScoreChange = rootDepth;
                        }
                    }

                    // =========================================================================
                    // SIGNAL 2: Update PV Stability (check first 4 moves of PV)
                    // =========================================================================
                    bool pvChanged = false;
                    for (int i = 0; i < 4 && i < bestRM.pv.length; ++i) {
                        if (bestRM.pv.moves[i] != previousPV[i]) {
                            pvChanged = true;
                            break;
                        }
                    }

                    if (!pvChanged && bestMoveStability >= 1) {
                        pvStability = std::min(pvStability + 1, 10);
                    } else {
                        pvStability = 0;
                    }

                    for (int i = 0; i < 4; ++i) {
                        previousPV[i] = (i < bestRM.pv.length) ? bestRM.pv.moves[i] : MOVE_NONE;
                    }

                    // =========================================================================
                    // SIGNAL 3: Calculate Score Fluctuation
                    // =========================================================================
                    int maxScore = -VALUE_INFINITE, minScore = VALUE_INFINITE;
                    int validScoreCount = 0;
                    for (int i = 0; i < 6; ++i) {
                        if (scoreHistory[i] != VALUE_NONE) {
                            maxScore = std::max(maxScore, scoreHistory[i]);
                            minScore = std::min(minScore, scoreHistory[i]);
                            validScoreCount++;
                        }
                    }
                    int scoreFluctuation = (validScoreCount >= 3) ? (maxScore - minScore) : 100;
                    bool scoreStable = scoreFluctuation < 30;

                    // =========================================================================
                    // SIGNAL 4: Calculate Stability Factor
                    // =========================================================================
                    double stabilityFactor = 1.0 - (bestMoveStability * 0.05);
                    stabilityFactor = std::max(stabilityFactor, 0.5);
                    int effectiveOptimum = static_cast<int>(optimumTime * stabilityFactor);

                    // =========================================================================
                    // MINIMUM DEPTH CHECK
                    // =========================================================================
                    const int MIN_DEPTH_FOR_EARLY_STOP = 10;

                    // =========================================================================
                    // COMBINED SIGNAL EARLY STOP DECISION
                    // =========================================================================
                    if (multiPV == 1) {
                        bool isMateScore = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;

                        if (!isMateScore && rootDepth >= MIN_DEPTH_FOR_EARLY_STOP) {
                            bool recentBigChange = (rootDepth - lastBigScoreChange) <= 3;
                            if (!recentBigChange) {
                                if (bestMoveStability >= 5 && pvStability >= 3 && scoreStable &&
                                    elapsed > effectiveOptimum * 0.6) {
                                    break;
                                }

                                if (bestMoveStability >= 4 && scoreStable &&
                                    elapsed > effectiveOptimum * 0.7) {
                                    break;
                                }

                                if (!failingLow && elapsed > effectiveOptimum * 0.85) {
                                    break;
                                }
                                if (failingLow && elapsed > optimumTime * 0.95) {
                                    break;
                                }
                            }
                        } else if (isMateScore) {
                            int mateIn = (score > 0) ?
                                (VALUE_MATE - score + 1) / 2 :
                                std::abs((VALUE_MATE + score) / 2);

                            if (rootDepth >= MIN_DEPTH_FOR_EARLY_STOP &&
                                mateIn <= 4 && bestMoveStability >= 3 &&
                                elapsed > optimumTime * 0.7) {
                                break;
                            }
                        }
                    }
                }

                previousRootScore = score;
                previousRootBestMove = rootBestMove;
            }
            else if (emergencyMode) {
                auto now = std::chrono::steady_clock::now();
                int elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                );

                if (rootDepth >= 4 && elapsed > optimumTime) {
                    break;
                }

                previousRootScore = bestRM.score;
                previousRootBestMove = rootBestMove;
            }
        }
    }
}

// ============================================================================
// Principal Variation Search
// ============================================================================

int Search::search(Board& board, int alpha, int beta, int depth, bool cutNode) {
    PROFILE_FUNCTION();
    const bool pvNode = (beta - alpha) > 1;

    int ply = board.game_ply() - rootPly;

    if (UNLIKELY(ply >= MAX_PLY)) {
        return evaluate(board);
    }
    if (ply > searchStats.selDepth) {
        searchStats.selDepth = ply;
    }

    TT.prefetch(board.key());

    if (UNLIKELY((searchStats.nodes & 16383) == 0)) {
        check_time();
    }

    if (UNLIKELY(stopped)) return 0;

    pvLines[ply].clear();

    SearchStack* ss = &stack[ply + 2];

    ss->cutoffCnt = 0;

    if (depth <= 0) {
        return qsearch(board, alpha, beta, 0);
    }

    ++searchStats.nodes;

    if (UNLIKELY(ply > 0 && board.is_draw(ply))) {
        int contempt = get_contempt(board);
        return -contempt;
    }

    alpha = std::max(alpha, -VALUE_MATE + ply);
    beta = std::min(beta, VALUE_MATE - ply - 1);
    if (UNLIKELY(alpha >= beta)) {
        return alpha;
    }

    if (ply + 1 < MAX_PLY) {
        pvLines[ply + 1].clear();
    }
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;
    if (!ttHit && ttMove != MOVE_NONE) {
    }

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
                pvLines[ply].length = 1;
                pvLines[ply].moves[0] = ttMove;
            }
            return ttScore;
        }
    }

    int staticEval;
    int correctedStaticEval;
    bool inCheck = board.in_check();
    Color us = board.side_to_move();

    if (inCheck) {
        staticEval = VALUE_NONE;
        correctedStaticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        staticEval = tte->eval();
        int correction = corrHistory.get(us, board.pawn_key());
        correctedStaticEval = staticEval + correction;
    } else {
        staticEval = evaluate(board, alpha, beta);
        int correction = corrHistory.get(us, board.pawn_key());
        correctedStaticEval = staticEval + correction;
    }

    ss->staticEval = staticEval;
    ss->correctedStaticEval = correctedStaticEval;

    bool improving = false;
    bool improving4ply = false;
    int improvementDelta = 0;

    if (!inCheck && ss->staticEval != VALUE_NONE) {
        if (ply >= 2 && stack[ply].staticEval != VALUE_NONE) {
            if (ss->staticEval >= stack[ply].staticEval) {
                improving = true;
                improvementDelta = ss->staticEval - stack[ply].staticEval;
            }
        } else {
            improving = true;
        }

        if (ply >= 4 && stack[ply - 2].staticEval != VALUE_NONE) {
            if (ss->staticEval >= stack[ply - 2].staticEval) {
                improving4ply = true;
            }
        }
    }

    if (inCheck) improving = true;

    if (!pvNode && !inCheck && depth <= 3 && depth >= 1) {
        int predictedDepth = std::max(1, depth - 1);
        int razorMarg = razoring_margin(predictedDepth);

        if (improving) razorMarg += 50;

        if (correctedStaticEval + razorMarg <= alpha) {
            int razorScore = qsearch(board, alpha - razorMarg, alpha - razorMarg + 1);
            if (razorScore <= alpha - razorMarg) {
                return razorScore;
            }
        }
    }

    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMarg = rfp_margin(depth, improving);

        if (correctedStaticEval - rfpMarg >= beta && correctedStaticEval < VALUE_MATE_IN_MAX_PLY) {
            return correctedStaticEval;
        }
    }

    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    bool doubleNullMove = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].nullMovePruned);
    bool mateThreat = false;

    if (!pvNode && !inCheck && correctedStaticEval >= beta && depth >= 3 &&
        hasNonPawnMaterial && !doubleNullMove) {

        int R = 3 + depth / 4 + std::min(3, (correctedStaticEval - beta) / 200);
        if (!improving) R++;

        StateInfo si;
        board.do_null_move(si);

        ss->nullMovePruned = true;

        int nullScore = -search(board, -beta, -beta + 1, depth - R - 1, !cutNode);

        board.undo_null_move();

        ss->nullMovePruned = false;

        if (UNLIKELY(stopped)) return 0;

        if (LIKELY(nullScore >= beta)) {
            if (UNLIKELY(nullScore >= VALUE_MATE_IN_MAX_PLY)) {
                nullScore = beta;
            }
            if (depth >= NULL_MOVE_VERIFY_DEPTH) {
                int verifyScore = search(board, beta - 1, beta, depth - R - 1, false);
                if (verifyScore >= beta) {
                    return nullScore;
                }
            } else {
                return nullScore;
            }
        }
        else if (depth >= MATE_THREAT_EXT_MIN_DEPTH && nullScore <= VALUE_MATED_IN_MAX_PLY) {
            mateThreat = true;
        }
    }

    if (!pvNode && !inCheck && depth >= MULTI_CUT_DEPTH && cutNode && ttMove != MOVE_NONE) {
        int multiCutCount = 0;
        int movesTried = 0;

        MovePicker mcPicker(board, ttMoves, ttMoveCount, ply, killers, counterMoves, history, previousMove,
                            nullptr, nullptr, &captureHist);
        Move m;

        while ((m = mcPicker.next_move()) != MOVE_NONE && movesTried < MULTI_CUT_COUNT + 2) {
            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            ++movesTried;

            StateInfo si;
            board.do_move(m, si);

            int mcDepth = depth - 1 - MULTI_CUT_DEPTH / 2;
            int mcScore = -search(board, -beta, -beta + 1, mcDepth, !cutNode);

            board.undo_move(m);

            if (stopped) return 0;

            if (mcScore >= beta) {
                ++multiCutCount;
                if (multiCutCount >= MULTI_CUT_REQUIRED) {
                    return beta;
                }
            }

            if (movesTried >= MULTI_CUT_COUNT) {
                break;
            }
        }
    }

    if (!pvNode && !inCheck && depth >= PROBCUT_DEPTH &&
        std::abs(beta) < VALUE_MATE_IN_MAX_PLY) {

        int probCutBeta = beta + PROBCUT_MARGIN;
        int probCutDepth = depth - 4;

        MoveList captures;
        MoveGen::generate_captures(board, captures);

        for (size_t i = 0; i < captures.size(); ++i) {
            Move m = captures[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            if (!SEE::see_ge(board, m, 0)) {
                continue;
            }

            StateInfo si;
            board.do_move(m, si);

            int qScore = -qsearch(board, -probCutBeta, -probCutBeta + 1, 0);

            if (qScore >= probCutBeta) {
                int probCutScore = -search(board, -probCutBeta, -probCutBeta + 1,
                                           probCutDepth, !cutNode);

                board.undo_move(m);

                if (stopped) return 0;
                if (probCutScore >= probCutBeta) {
                    return probCutScore;
                }
            } else {
                board.undo_move(m);
            }

            if (stopped) return 0;
        }
    }

    int searchDepth = depth;
    if (!ttMove && depth >= IIR_MIN_DEPTH) {
        if (pvNode) {
            searchDepth -= IIR_PV_REDUCTION;
        } else if (cutNode) {
            searchDepth -= IIR_CUT_REDUCTION;
        } else {
            searchDepth -= IIR_REDUCTION;
        }
    }

    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

    Move capturesSearched[32];
    Piece capturePieces[32];
    PieceType capturedTypes[32];
    int captureSEE[32];
    int captureCount = 0;
    bool singularSearched = false;

    const bool rootNode = (ply == 0);

    const ContinuationHistoryEntry* contHist1ply = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].contHistory) ?
                                                    stack[ply + 1].contHistory : nullptr;
    const ContinuationHistoryEntry* contHist2ply = (ply >= 2 && ply < MAX_PLY + 4 && stack[ply].contHistory) ?
                                                    stack[ply].contHistory : nullptr;

    MovePicker mp(board, ttMoves, ttMoveCount, ply, killers, counterMoves, history, previousMove,
                  contHist1ply, contHist2ply, &captureHist);

    size_t rootMoveIdx = 0;
    Move m;

    while (true) {
        if (rootNode) {
            if (rootMoveIdx + pvIdx >= rootMoves.size()) {
                break;
            }
            m = rootMoves[rootMoveIdx + pvIdx].move;
            ++rootMoveIdx;
        } else {
            m = mp.next_move();
            if (m == MOVE_NONE) {
                break;
            }
        }

        if (m == ss->excludedMove) {
            continue;
        }

        if (!MoveGen::is_legal(board, m)) {
            continue;
        }

        ++moveCount;

        bool isCapture = !board.empty(m.to()) || m.is_enpassant();
        bool isPromotion = m.is_promotion();
        bool givesCheck = MoveGen::gives_check(board, m);
        bool isTTMove = (m == ttMove);

        Piece movedPiece = board.piece_on(m.from());
        PieceType movedPt = type_of(movedPiece);
        Color us = board.side_to_move();

        bool createsThreat = false;
        bool createsFork = false;
        bool escapesAttack = false;
        bool threatDetectionDone = false;

        auto computeThreatDetection = [&]() {
            if (threatDetectionDone) return;
            threatDetectionDone = true;

            Bitboard attackersToFrom = board.attackers_to(m.from(), board.pieces()) & board.pieces(~us);
            if (attackersToFrom) {
                int pieceValueMoving = PieceValue[movedPt];
                int minAttackerValue = 20000;
                for (Bitboard atk = attackersToFrom; atk; ) {
                    Square atkSq = pop_lsb(atk);
                    PieceType atkPt = type_of(board.piece_on(atkSq));
                    minAttackerValue = std::min(minAttackerValue, PieceValue[atkPt]);
                }

                if (movedPt == QUEEN || movedPt == ROOK || pieceValueMoving >= minAttackerValue) {
                    escapesAttack = true;
                }
            }

            if (!isCapture && !givesCheck) {
                Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
                Bitboard attacksAfter = attacks_bb(movedPt, m.to(), newOccupied);
                Bitboard valuableEnemies = board.pieces(~us, QUEEN) | board.pieces(~us, ROOK);
                if (attacksAfter & valuableEnemies) {
                    createsThreat = true;
                }

                if (movedPt == KNIGHT || movedPt == BISHOP) {
                    Bitboard allMinorsAndUp = valuableEnemies | board.pieces(~us, BISHOP) | board.pieces(~us, KNIGHT);
                    if (popcount(attacksAfter & allMinorsAndUp) >= 2) {
                        createsFork = true;
                        createsThreat = true;
                    }
                }

                if (movedPt == QUEEN || movedPt == ROOK) {
                    Square enemyKingSq = board.king_square(~us);
                    Bitboard kingZone = king_attacks_bb(enemyKingSq);

                    if (attacksAfter & kingZone) {
                        createsThreat = true;
                    }

                    if (attacksAfter & square_bb(enemyKingSq)) {
                        createsThreat = true;
                    }
                }
            }
        };

        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            !givesCheck && bestScore > VALUE_MATED_IN_MAX_PLY) {
            int lmpThresh = lmp_threshold(depth, improving);
            if (moveCount > lmpThresh) {
                if (!killers.is_killer(ply, m) && history.get(us, m) < 0) {
                    continue;
                }
            }
        }

        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck) {
            int futilMarg = futility_margin(depth, improving);
            if (correctedStaticEval + futilMarg <= alpha) {
                if (!killers.is_killer(ply, m) && !isTTMove) {
                    continue;
                }
            }
        }

        if (!pvNode && depth <= 4 && isCapture && !givesCheck) {
            Piece capturedPiece = board.piece_on(m.to());
            PieceType capturedType = (capturedPiece != NO_PIECE) ? type_of(capturedPiece) :
                                     (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);

            int seeThreshold;
            if (capturedType >= PAWN && capturedType <= QUEEN) {
                seeThreshold = improving ? -100 * depth : -80 * depth;
            } else {
                seeThreshold = improving ?
                    -SEE_CAPTURE_IMPROVING_FACTOR * depth :
                    -SEE_CAPTURE_NOT_IMPROVING_FACTOR * depth;
            }

            if (!SEE::see_ge(board, m, seeThreshold)) {
                continue;
            }
        }

        if (!pvNode && !inCheck && depth <= 3 && !isCapture && !givesCheck) {
            int seeThreshold = improving ?
                -SEE_QUIET_IMPROVING_FACTOR * depth :
                -SEE_QUIET_NOT_IMPROVING_FACTOR * depth;
            if (!SEE::see_ge(board, m, seeThreshold)) {
                continue;
            }
        }

        if (!pvNode && !inCheck && depth <= HISTORY_LEAF_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY && moveCount > 4) {
            int histScore = history.get(board.side_to_move(), m);
            if (histScore < -HISTORY_LEAF_PRUNING_MARGIN * depth) {
                continue;
            }
        }

        if (!pvNode && !inCheck && depth <= COUNTER_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY && contHist1ply && moveCount > 5) {

            PieceType pt = type_of(movedPiece);
            int cmHistScore = contHist1ply->get(pt, m.to());

            if (cmHistScore < -COUNTER_HIST_PRUNING_MARGIN * depth) {
                continue;
            }
        }

        if (!pvNode && !inCheck && depth <= FOLLOWUP_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY && ply >= 4 && stack[ply - 2].contHistory && moveCount > 6) {

            const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
            PieceType pt = type_of(movedPiece);
            int followupScore = contHist4ply->get(pt, m.to());

            if (followupScore < -FOLLOWUP_HIST_PRUNING_MARGIN * depth) {
                continue;
            }
        }

        int extension = 0;
        int currentExtensions = (ss->ply >= 2 && ply >= 2) ? stack[ply + 1].extensions : 0;

        int doubleExtensions = (ply >= 1) ? stack[ply + 1].doubleExtensions : 0;

        if (givesCheck && currentExtensions < MAX_EXTENSIONS) {
            if (SEE::see_ge(board, m, 0)) {
                extension = 1;
            }
        }

        if (inCheck && currentExtensions < MAX_EXTENSIONS) {
            extension = std::max(extension, 1);
        }

        if (movedPt == PAWN && currentExtensions < MAX_EXTENSIONS) {
            Rank toRank = relative_rank(us, m.to());
            if (toRank == RANK_7) {
                extension = std::max(extension, 1);
            }
        }

        // =====================================================================
        // MATE THREAT EXTENSION
        // =====================================================================
        if (mateThreat && currentExtensions < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        // =====================================================================
        // PV EXTENSION
        // =====================================================================
        if (pvNode && moveCount == 1 && depth >= PV_EXT_MIN_DEPTH &&
            currentExtensions < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        // =====================================================================
        // CAPTURE EXTENSION - Extend important tactical captures
        // Optimized: single SEE call, only for important captures at high depth
        // =====================================================================
        if (isCapture && depth >= CAPTURE_EXT_MIN_DEPTH && currentExtensions < MAX_EXTENSIONS && !m.is_enpassant()) {
            Piece captured = board.piece_on(m.to());
            PieceType capturedPt = type_of(captured);

            bool shouldExtend = (capturedPt == QUEEN) ||
                               (previousMove != MOVE_NONE && m.to() == previousMove.to());

            if (shouldExtend && SEE::see_ge(board, m, 0)) {
                extension = std::max(extension, 1);
            }
        }

        // =====================================================================
        // KING ATTACK EXTENSION - Extend Queen/Rook moves attacking king zone
        // Only at high depths to reduce bitboard overhead
        // =====================================================================
        if (!isCapture && !givesCheck && currentExtensions < MAX_EXTENSIONS &&
            extension == 0 && depth >= 8 && (movedPt == QUEEN || movedPt == ROOK)) {

            Square enemyKingSq = board.king_square(~us);
            Bitboard kingZone = king_attacks_bb(enemyKingSq);
            Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
            Bitboard attacksAfter = attacks_bb(movedPt, m.to(), newOccupied);

            if (attacksAfter & (kingZone | square_bb(enemyKingSq))) {
                int enemyMaterial = popcount(board.pieces(~us)) - 2;
                if (enemyMaterial <= 4) {
                    extension = 1;
                }
            }
        }

        // =====================================================================
        // SINGULAR EXTENSION - Enhanced with dynamic margins
        // =====================================================================
        if (!singularSearched && depth >= SINGULAR_DEPTH && isTTMove &&
            ttHit && ttBound != BOUND_UPPER && ttDepth >= depth - 3 &&
            std::abs(ttScore) < VALUE_MATE_IN_MAX_PLY && currentExtensions < MAX_EXTENSIONS) {

            singularSearched = true;

            int singularMargin = SINGULAR_MARGIN;

            int depthDiff = depth - ttDepth;
            if (depthDiff > 0) {
                singularMargin += depthDiff * SINGULAR_TT_DEPTH_PENALTY;
            }

            if (!improving) {
                singularMargin -= SINGULAR_IMPROVING_BONUS;
            }

            if (pvNode) {
                singularMargin -= 5;
            }

            int singularBeta = std::max(ttScore - singularMargin * depth / 8, -VALUE_MATE);
            int singularDepth = (depth - 1) / 2;

            ss->excludedMove = m;
            int singularScore = search(board, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = MOVE_NONE;

            if (singularScore < singularBeta) {
                extension = 1;

                int doubleExtThreshold = SINGULAR_DOUBLE_EXT_BASE + (depth / 4) * 10;

                if (!pvNode && singularScore < singularBeta - doubleExtThreshold &&
                    doubleExtensions < DOUBLE_EXT_LIMIT) {
                    extension = 2;
                    ++doubleExtensions;
                }
            } else if (singularBeta >= beta) {
                return singularBeta;
            }

            else if (cutNode && depth >= NEG_EXT_MIN_DEPTH &&
                     singularScore < alpha - NEG_EXT_THRESHOLD) {
                extension = 1;
            }
        }

        if (extension > 0 && ply >= rootDepth * MAX_EXTENSION_PLY_RATIO) {
            extension = 0;
        }

        int newDepth = searchDepth - 1 + extension;

        if (ss->ply >= 0 && ss->ply < MAX_PLY) {
            stack[ply + 2].extensions = currentExtensions + extension;
            stack[ply + 2].doubleExtensions = doubleExtensions;
        }

        int reduction = 0;
        if (depth >= 2 && moveCount > 1 && !isCapture && !isPromotion && !givesCheck) {
            reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];
            int delta = beta - alpha;
            if (delta < 20) {
                reduction += 1;
            }

            if (cutNode) {
                reduction += LMR_CUTNODE_BONUS;
                if (!ttMove) {
                    reduction += 1;
                }
            }

            if (!improving) {
                reduction += 1;
            }

            if (ply + 2 < MAX_PLY + 4) {
                int childCutoffs = stack[ply + 2].cutoffCnt;
                if (childCutoffs > 1) {
                    reduction += LMR_CUTOFF_CNT_BONUS;
                }
                if (childCutoffs > 2) {
                    reduction += LMR_CUTOFF_CNT_BONUS;
                }
            }

            if (moveCount > 10) {
                reduction += 1;
            }
            if (moveCount > 18) {
                reduction += 1;
            }

            if (pvNode) {
                reduction -= 2;
            }

            if (inCheck) {
                reduction -= 1;
            }

            if (killers.is_killer(ply, m) ||
                (previousMove && m == counterMoves.get(board.piece_on(previousMove.to()), previousMove.to()))) {
                reduction -= 2;
            }

            if (isTTMove) {
                reduction -= 2;
            }

            int statScore = 2 * history.get(board.side_to_move(), m);

            PieceType pt = type_of(movedPiece);
            if (contHist1ply) {
                statScore += contHist1ply->get(pt, m.to());
            }
            if (contHist2ply) {
                statScore += contHist2ply->get(pt, m.to());
            }
            if (ply >= 4 && stack[ply - 2].contHistory) {
                const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
                statScore += contHist4ply->get(pt, m.to());
            }

            reduction -= statScore / 4096;

            if (cutNode && !ttMove && depth >= 6) {
                reduction += reduction / (depth + 1);
            }

            reduction = std::clamp(reduction, 0, newDepth - 1);
        }

        if (ply + 2 < MAX_PLY + 4) {
            stack[ply + 2].contHistory = contHistory.get_entry(movedPiece, m.to());
        }

        StateInfo si;
        board.do_move(m, si);

        TT.prefetch(board.key());

        U64 nodesBefore = 0;
        if (rootNode) {
            nodesBefore = searchStats.nodes;
        }

        int score;

        if (moveCount == 1) {
            score = -search(board, -beta, -alpha, newDepth, false);
        } else {
            score = -search(board, -alpha - 1, -alpha, newDepth - reduction, true);

            if (score > alpha && reduction > 0) {
                score = -search(board, -alpha - 1, -alpha, newDepth, !cutNode);
            }

            if (score > alpha && score < beta) {
                score = -search(board, -beta, -alpha, newDepth, false);
            }
        }

        board.undo_move(m);

        if (rootNode) {
            U64 nodesAfter = searchStats.nodes;
            U64 nodeDiff = nodesAfter - nodesBefore;

            for (auto& rm : rootMoves) {
                if (rm.move == m) {
                    rm.subtreeNodes += nodeDiff;
                    break;
                }
            }
        }

        if (UNLIKELY(stopped)) {
            return 0;
        }

        if (!isCapture && quietCount < 64) {
            quietsSearched[quietCount++] = m;
        }
        if (isCapture && captureCount < 32) {
            capturesSearched[captureCount] = m;
            capturePieces[captureCount] = movedPiece;
            Piece captured = board.piece_on(m.to());
            capturedTypes[captureCount] = (captured != NO_PIECE) ? type_of(captured) :
                                          (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);
            captureSEE[captureCount] = SEE::evaluate(board, m);
            captureCount++;
        }

        if (score > bestScore) {
            bestScore = score;
            bestMove = m;

            if (score > alpha) {
                if (ply + 1 < MAX_PLY) {
                    pvLines[ply].update(m, pvLines[ply + 1]);
                }

                if (rootNode) {
                    for (auto& rm : rootMoves) {
                        if (rm.move == m) {
                            rm.score = score;
                            rm.selDepth = searchStats.selDepth;
                            rm.pv = pvLines[ply];
                            break;
                        }
                    }
                }

                if (score >= beta) {
                    // =========================================================
                    // BETA CUTOFF - Depth-dependent bonus for the winning move
                    // Using stat_bonus/stat_malus formulas for better tuning
                    // =========================================================

                    int statBonus = std::min(105 + 175 * depth + 11 * depth * depth, 2400);
                    int statMalus = std::min(80 + 145 * depth + 8 * depth * depth, 1900);

                    if (!isCapture) {
                        killers.store(ply, m);

                        if (score >= VALUE_MATE_IN_MAX_PLY) {
                            mateKillers.store(ply, m);
                        }

                        if (previousMove) {
                            counterMoves.store(board.piece_on(m.from()), m.to(), m);
                        }

                        history.update_quiet_stats(board.side_to_move(), m,
                                                   quietsSearched, quietCount - 1, depth);

                        PieceType pt = type_of(movedPiece);

                        if (contHist1ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), statBonus * 2);
                        }
                        if (contHist2ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(pt, m.to(), statBonus);
                        }
                        if (ply >= 4 && stack[ply - 2].contHistory) {
                            const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
                            const_cast<ContinuationHistoryEntry*>(contHist4ply)->update(pt, m.to(), statBonus);
                        }

                        for (int i = 0; i < quietCount - 1; ++i) {
                            if (quietsSearched[i] != m) {
                                Piece qpc = board.piece_on(quietsSearched[i].from());
                                PieceType qpt = type_of(qpc);
                                Square qto = quietsSearched[i].to();

                                history.update(board.side_to_move(), quietsSearched[i], depth, false);

                                if (contHist1ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -statMalus * 2);
                                }
                                if (contHist2ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -statMalus);
                                }
                            }
                        }
                    } else {
                        Piece captured = board.piece_on(m.to());
                        PieceType capturedPt = (captured != NO_PIECE) ? type_of(captured) :
                                               (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);
                        if (capturedPt != NO_PIECE_TYPE) {
                            captureHist.update_capture_stats(movedPiece, m.to(), capturedPt, depth, true);
                        }
                    }

                    for (int i = 0; i < captureCount; ++i) {
                        if (capturesSearched[i] != m && capturedTypes[i] != NO_PIECE_TYPE) {
                            int penaltyDepth = depth;
                            if (captureSEE[i] < 0) {
                                penaltyDepth = depth * 2;
                            }

                            captureHist.update_capture_stats(capturePieces[i],
                                                             capturesSearched[i].to(),
                                                             capturedTypes[i],
                                                             penaltyDepth, false);
                        }
                    }

                    ss->cutoffCnt++;

                    break;
                }

                // =========================================================
                // IMPROVED ALPHA but no cutoff - Scaled bonus
                // =========================================================
                {
                    // Use scaled stat_bonus for alpha improvement (1/4 of full bonus)
                    int smallBonus = std::min((105 + 175 * depth + 11 * depth * depth) / 4, 600);

                    if (!isCapture) {
                        // Small bonus to best move history
                        history.update_with_bonus(board.side_to_move(), m, smallBonus);

                        PieceType pt = type_of(movedPiece);
                        if (contHist1ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), smallBonus);
                        }
                        if (contHist2ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(pt, m.to(), smallBonus / 2);
                        }
                    }
                }

                alpha = score;
            }
        }
    }

    // =========================================================
    // END OF MOVE LOOP - Penalty for moves that failed to raise alpha
    // (Only applies if we didn't cutoff and have a best move)
    // =========================================================
    if (bestMove != MOVE_NONE && bestScore > -VALUE_INFINITE) {
        int penalty = depth;

        bool bestIsCapture = !board.empty(bestMove.to()) || bestMove.is_enpassant();

        if (!bestIsCapture) {

            for (int i = 0; i < quietCount; ++i) {
                if (quietsSearched[i] == bestMove) continue;

                Piece qpc = board.piece_on(quietsSearched[i].from());
                PieceType qpt = type_of(qpc);
                Square qto = quietsSearched[i].to();

                if (contHist1ply) {
                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -penalty);
                }
                if (contHist2ply) {
                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -penalty / 2);
                }
            }
        }

        for (int i = 0; i < captureCount; ++i) {
            if (capturesSearched[i] != bestMove && capturedTypes[i] != NO_PIECE_TYPE) {
                int penaltyDepth = depth / 2;

                if (captureSEE[i] < 0) {
                    penaltyDepth = depth;
                }

                captureHist.update_capture_stats(capturePieces[i],
                                                 capturesSearched[i].to(),
                                                 capturedTypes[i],
                                                 penaltyDepth, false);
            }
        }
    }

    if (moveCount == 0) {
        if (inCheck) {
            return -VALUE_MATE + ply;
        } else {
            return 0;
        }
    }

    Bound bound = bestScore >= beta ? BOUND_LOWER :
                  bestScore > alpha ? BOUND_EXACT : BOUND_UPPER;

    if (!stopped && tte) {
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation());
    }

    if (!inCheck && staticEval != VALUE_NONE && depth >= 3 &&
        std::abs(bestScore) < VALUE_MATE_IN_MAX_PLY) {

        int diff = bestScore - staticEval;
        diff = std::clamp(diff, -500, 500);

        if (bound == BOUND_EXACT ||
            (bound == BOUND_LOWER && bestScore >= beta)) {
            corrHistory.update(us, board.pawn_key(), diff, depth);
        }
    }

    return bestScore;
}

int Search::qsearch(Board& board, int alpha, int beta, int qsDepth, Square recaptureSquare) {
    PROFILE_FUNCTION();
    ++searchStats.nodes;

    if (UNLIKELY((searchStats.nodes & 16383) == 0)) {
        check_time();
    }

    if (UNLIKELY(stopped)) return 0;

    int ply = board.game_ply() - rootPly;

    if (UNLIKELY(ply >= MAX_PLY)) {
        return evaluate(board);
    }

    // Prefetch TT entry early to hide memory latency
    TT.prefetch(board.key());

    pvLines[ply].clear();

    bool inCheck = board.in_check();

    int legalMoveCount = 0;

    int staticEval = inCheck ? -VALUE_INFINITE : evaluate(board, alpha, beta);

    if (LIKELY(!inCheck)) {
        if (LIKELY(staticEval >= beta)) {
            return staticEval;
        }
        if (staticEval > alpha) {
            alpha = staticEval;
        }
    }

    MoveList moves;
    MoveList quietChecks;

    if (inCheck) {
        MoveGen::generate_evasions(board, moves);
    } else {
        MoveGen::generate_captures(board, moves);

        if (qsDepth >= QSEARCH_CHECK_DEPTH && qsDepth >= 0) {
            MoveGen::generate_checking_moves(board, quietChecks);
        }
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

    if (ttHit && !inCheck) {
        int ttScore = score_from_tt(tte->score(), ply);
        int ttDepth = tte->depth();
        Bound ttBound = tte->bound();

        if (ttDepth >= 0) {
            bool isMateScore = std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY;

            if (!isMateScore) {
                if (ttBound == BOUND_EXACT) {
                    return ttScore;
                }
                if (ttBound == BOUND_LOWER && ttScore >= beta) {
                    return ttScore;
                }
                if (ttBound == BOUND_UPPER && ttScore <= alpha) {
                    return ttScore;
                }
            }
        }
    }

    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;
    Move m;

    if (inCheck) {
        for (size_t i = 0; i < moves.size(); ++i) {
            m = moves[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            ++legalMoveCount;

            StateInfo si;
            board.do_move(m, si);

            int score = -qsearch(board, -beta, -alpha, qsDepth, SQ_NONE);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;
                    }
                    alpha = score;
                }
            }
        }
    } else {
        MovePicker mp(board, ttMoves, ttMoveCount, history, &captureHist);

        while ((m = mp.next_move()) != MOVE_NONE) {
            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            PieceType capturedPt = NO_PIECE_TYPE;
            int captureValue = 0;

            if (!m.is_enpassant()) {
                capturedPt = type_of(board.piece_on(m.to()));
                captureValue = PieceValue[capturedPt];
            } else {
                capturedPt = PAWN;
                captureValue = PieceValue[PAWN];
            }

            bool captureGivesCheck = MoveGen::gives_check(board, m);

            if (!m.is_promotion() && !captureGivesCheck) {
                if (capturedPt != QUEEN && capturedPt != ROOK) {
                if (staticEval + captureValue + DELTA_PRUNING_MARGIN < alpha) {
                        continue;
                    }
                }
            }

            if (capturedPt != QUEEN && !captureGivesCheck) {
                if (!SEE::see_ge(board, m, -1)) {
                    continue;
                }
            }

            StateInfo si;
            board.do_move(m, si);

            // Prefetch TT for the new position
            TT.prefetch(board.key());

            bool isRecapture = (recaptureSquare != SQ_NONE && m.to() == recaptureSquare);
            bool isCapture = (capturedPt != NO_PIECE_TYPE) || m.is_enpassant();

            int newQsDepth = isRecapture ? qsDepth : qsDepth - 1;
            Square newRecaptureSquare = isCapture ? m.to() : SQ_NONE;

            int score = -qsearch(board, -beta, -alpha, newQsDepth, newRecaptureSquare);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;
                    }
                    alpha = score;
                }
            }
        }
    }

    if (!inCheck) {
        for (size_t i = 0; i < quietChecks.size(); ++i) {
            m = quietChecks[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            if (!SEE::see_ge(board, m, 0)) {
                continue;
            }
            StateInfo si;
            board.do_move(m, si);

            int score = -qsearch(board, -beta, -alpha, qsDepth - 1, SQ_NONE);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;
                    }
                    alpha = score;
                }
            }
        }
    }

    if (inCheck && legalMoveCount == 0) {
        return -VALUE_MATE + ply;
    }

    return bestScore;
}

int Search::evaluate(const Board& board) {
    PROFILE_SCOPE("evaluate");
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

int Search::evaluate(const Board& board, int alpha, int beta) {
    if (Tablebase::EndgameRules::is_known_draw(board)) {
        return 0;
    }

    int score = Eval::evaluate(board, alpha, beta);

    int scaleFactor = Tablebase::EndgameRules::scale_factor(board);
    if (scaleFactor != 128) {
        score = score * scaleFactor / 128;
    }

    return score;
}

// Quiescence search wrapper for datagen quiet position detection
// Returns the qsearch score from side-to-move's perspective
int Search::qsearch_score(Board& board) {
    // Initialize all necessary search state for qsearch
    rootPly = board.game_ply();
    stopped = false;
    searchStats.reset();

    // Initialize PV lines to prevent crashes
    for (int i = 0; i < MAX_PLY; ++i) {
        pvLines[i].clear();
    }

    // Run qsearch with full window
    int score = qsearch(board, -VALUE_INFINITE, VALUE_INFINITE, 0, SQ_NONE);

    // Score is already from side-to-move's perspective
    // Convert to white's perspective for consistency with evaluate()
    if (board.side_to_move() == BLACK) {
        score = -score;
    }

    return score;
}

void Search::report_info(Board& board, int depth, int score, const PVLine& pv, int multiPVIdx) {
    auto now = std::chrono::steady_clock::now();
    U64 elapsed = static_cast<U64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed == 0) elapsed = 1;

    U64 nps = searchStats.nodes * 1000 / elapsed;

    std::cout << "info";
    std::cout << " depth " << depth;
    std::cout << " seldepth " << searchStats.selDepth;

    if (UCI::options.multiPV > 1) {
        std::cout << " multipv " << multiPVIdx;
    }

    if (std::abs(score) >= VALUE_MATE_IN_MAX_PLY) {
        int mateIn = (score > 0) ?
            (VALUE_MATE - score + 1) / 2 :
            -(VALUE_MATE + score) / 2;
        std::cout << " score mate " << mateIn;
    } else {
        std::cout << " score cp " << score;
    }

    std::cout << " nodes " << searchStats.nodes;
    std::cout << " nps " << nps;
    std::cout << " time " << elapsed;
    std::cout << " hashfull " << TT.hashfull();

    std::cout << " pv";
    Board tempBoard = board;
    int movesOutput = 0;
    for (int i = 0; i < pv.length && i < MAX_PLY; ++i) {
        Move m = pv.moves[i];
        if (m == MOVE_NONE) break;

        if (!MoveGen::is_pseudo_legal(tempBoard, m) || !MoveGen::is_legal(tempBoard, m)) {
            break;
        }

        std::cout << " " << move_to_string(m);
        movesOutput++;

        StateInfo si;
        tempBoard.do_move(m, si);
    }

    std::cout << std::endl;
    std::cout.flush();

    if (infoCallback) {
        SearchInfo info;
        info.depth = depth;
        info.selDepth = searchStats.selDepth;
        info.score = score;
        info.isMate = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;
        info.nodes = searchStats.nodes;
        info.time = elapsed;
        info.nps = nps;
        info.hashfull = TT.hashfull();
        info.multiPVIdx = multiPVIdx;
        info.pv = pv;
        infoCallback(info);
    }
}
