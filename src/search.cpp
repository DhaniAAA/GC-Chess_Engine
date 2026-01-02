#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include "uci.hpp"
#include "moveorder.hpp"
#include "search_constants.hpp"
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

int LMRTable[64][64];  // [depth][moveCount]

void init_lmr_table() {
    for (int depth = 0; depth < 64; ++depth) {
        for (int move = 0; move < 64; ++move) {
            // LMR tidak berlaku untuk langkah pertama atau kedalaman sangat rendah
            if (depth < 2 || move == 0) {
                LMRTable[depth][move] = 0;
                continue;
            }

            // RUMUS STOCKFISH LOG-LOG:
            // Reduction = Base + (ln(depth) * ln(move_count)) / Divisor
            double reduction = SearchParams::LMR_BASE + (std::log(depth) * std::log(move)) / SearchParams::LMR_DIVISOR;

            // Simpan sebagai integer (floor)
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
        // Dynamic contempt: adjust based on material balance
        // When we have more material, we want to avoid draws more (higher contempt)
        // When we have less material, we might accept draws (lower contempt)
        int materialBalance = Eval::material_balance(board);
        Color us = board.side_to_move();

        // If it's our turn and we're ahead, increase contempt (avoid draws)
        // If we're behind, decrease contempt (accept draws more)
        if (us == WHITE) {
            contempt += materialBalance / 20;  // Scale: every 200cp = +10 contempt
        } else {
            contempt -= materialBalance / 20;
        }

        // Clamp to reasonable range
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

    // Initialize search stack
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
    mateKillers.clear();  // Clear mate killers between games
    counterMoves.clear();
    history.clear();
    contHistory.clear();
    corrHistory.clear();  // Clear correction history between games
    captureHist.clear();  // Clear capture history using CaptureHistory class
    moveOrderStats.reset();  // Reset move ordering statistics
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
            // Report book move
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
    // Transition from ponder mode to normal search
    // This is called when the opponent plays the move we predicted

    // 1. Disable ponder mode
    isPondering = false;
    limits.ponder = false;

    // 2. Reset time management with actual time
    // The time limits were already set by the GUI, we just need to
    // reset the start time so we can track actual search time
    startTime = std::chrono::steady_clock::now();

    // Note: We do NOT reset the search - it continues from the current depth
    // This is the key benefit of pondering - we get to keep the work done
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
    positionComplexity = 50;  // Default medium complexity

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

    int safeTime = std::max(1, time_left - moveOverhead);

    if (time_left < 500) {

        emergencyMode = true;
        optimumTime = std::max(5, time_left / 20);   // 5% of remaining
        maximumTime = std::max(10, time_left / 10);  // 10% of remaining
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

    // Minimum time bounds
    optimumTime = std::max(optimumTime, 50);
    maximumTime = std::max(maximumTime, 100);
}

void Search::check_time() {
    if (stopped) return;

    // In ponder mode or infinite analysis, don't check time
    if (limits.infinite || limits.ponder || isPondering) return;

    // [FIX] When using 'go depth X' without time limits, don't stop based on time
    // Only depth limit will stop the search
    if (limits.depth > 0 && maximumTime == 0 && optimumTime == 0) {
        return;  // Depth-only search - no time limit
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
            // 1. Best Move Changed? (from previous iteration)
            if (previousRootBestMove != MOVE_NONE && rootBestMove != previousRootBestMove) {
                unstable = true;
            }

            // 2. Score Fluctuation? (Fail Low of previous best move)
            if (previousRootScore != VALUE_NONE) {
                // If the score of the primary candidate move has changed drastically
                int currentPvScore = rootMoves[0].score;

                // [PERBAIKAN] Saat mate score, jangan anggap fluktuasi sebagai instability
                // Karena mate distance bisa berubah saat kita menemukan variasi yang berbeda
                bool currentIsMate = std::abs(currentPvScore) >= VALUE_MATE_IN_MAX_PLY;
                bool previousIsMate = std::abs(previousRootScore) >= VALUE_MATE_IN_MAX_PLY;

                if (currentIsMate && previousIsMate) {
                    // Kedua score adalah mate - cek apakah masih dalam arah yang sama
                    // (keduanya winning mate atau keduanya losing mate)
                    bool sameDirection = (currentPvScore > 0) == (previousRootScore > 0);
                    if (!sameDirection) {
                        unstable = true;  // Berubah dari winning ke losing atau sebaliknya
                    }
                    // Jika arah sama, tidak dianggap unstable (mate distance mungkin berubah)
                } else if (!currentIsMate && !previousIsMate) {
                    // Normal score comparison
                    if (std::abs(currentPvScore - previousRootScore) > 40) { // 40cp threshold
                        unstable = true;
                    }
                } else {
                    // Transisi antara mate dan non-mate score - selalu unstable
                    unstable = true;
                }
            }
        }

        if (!unstable) {
            stopped = true;
        }
    }

    // Node limit check
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
    rootPly = board.game_ply();  // Store starting ply for relative ply calculation
    pvIdx = 0;

    // [FIX] Save a copy of root board position for use in report_info
    // This ensures we always validate PV moves against the correct position
    // even if the board reference gets modified somewhere
    Board rootBoard = board;

    rootMoves.clear();
    MoveList legalMoves;
    MoveGen::generate_legal(board, legalMoves);

    if (legalMoves.empty()) {
        return;  // No legal moves
    }

    // Populate rootMoves vector
    for (size_t i = 0; i < legalMoves.size(); ++i) {
        rootMoves.push_back(RootMove(legalMoves[i].move));
    }

    // Set fallback move to first legal move
    rootBestMove = rootMoves[0].move;

    // When there's only one legal move, still do a quick search to report evaluation
    // This helps GUI users see the position score even when there's no choice
    if (rootMoves.size() == 1 && !limits.infinite) {
        // Do a quick depth-1 search to get evaluation and update PV
        pvIdx = 0;
        rootDepth = 1;

        // Perform a quick search to get the score
        int score = search(board, -VALUE_INFINITE, VALUE_INFINITE, 1, false);

        if (!stopped) {
            // Update root move with result
            rootMoves[0].score = score;

            // Build minimal PV with just the forced move
            if (rootMoves[0].pv.length == 0) {
                rootMoves[0].pv.moves[0] = rootMoves[0].move;
                rootMoves[0].pv.length = 1;
            }

            // Report info to GUI so user sees evaluation
            report_info(rootBoard, 1, score, rootMoves[0].pv, 1);

            // Output string indicating forced move
            std::cout << "info string Only one legal move" << std::endl;
        }

        return;  // Only one legal move - no need for deep search
    }

    int maxDepth = limits.depth > 0 ? limits.depth : MAX_PLY;

    // Get MultiPV count from UCI options, clamped to legal move count
    int multiPV = std::min(UCI::options.multiPV, static_cast<int>(rootMoves.size()));
    multiPV = std::max(1, std::min(multiPV, MAX_MULTI_PV));

    // Initialize stability calculation members
    previousRootScore = VALUE_NONE;
    previousRootBestMove = MOVE_NONE;

    // Initialize Stockfish-style stability tracking
    for (int i = 0; i < 4; ++i) previousPV[i] = MOVE_NONE;
    for (int i = 0; i < 6; ++i) scoreHistory[i] = VALUE_NONE;
    scoreHistoryIdx = 0;
    pvStability = 0;
    lastBigScoreChange = 0;

    // Iterative deepening loop
    // [FIX] Track best known PV per move for fallback when deeper searches produce corrupt PV
    // Using map so we can restore the correct PV for each specific move
    std::unordered_map<uint16_t, std::pair<PVLine, int>> bestKnownPVPerMove;

    // Also track the overall best result (move + PV + score) for ultimate fallback
    Move overallBestMove = MOVE_NONE;
    PVLine overallBestPV;
    int overallBestScore = -VALUE_INFINITE;
    overallBestPV.clear();

    for (rootDepth = 1; rootDepth <= maxDepth && !stopped; ++rootDepth) {

        // Save previous scores and subtree nodes for all root moves ordering
        for (auto& rm : rootMoves) {
            rm.previousScore = rm.score;
            rm.prevSubtreeNodes = rm.subtreeNodes;
            rm.subtreeNodes = 0; // Reset for current iteration
        }

        // [FIX] Save snapshot of rootMoves before this depth for potential rollback
        // If the search at this depth produces unreliable results (empty PV for best move),
        // we can restore to the previous depth's reliable results
        std::vector<RootMove> rootMovesBackup = rootMoves;

        // MultiPV loop - search each PV line
        for (pvIdx = 0; pvIdx < multiPV && !stopped; ++pvIdx) {

            // [FIX] Save previous PV and score before aspiration search
            // This allows us to restore them if the new search produces suspicious results
            // (e.g., empty PV with drastically different score - sign of search instability)
            // NOTE: rm.score is from previous depth iteration, rm.previousScore is from 2 depths ago
            Move analyzedMove = rootMoves[pvIdx].move;  // The move we're about to search
            PVLine previousPVLine = rootMoves[pvIdx].pv;
            int previousIterScore = rootMoves[pvIdx].score;  // Use current score, not previousScore

            // Set aspiration window based on previous score
            // Using exponential widening: delta * 3 on each retry
            int alpha = -VALUE_INFINITE;
            int beta = VALUE_INFINITE;
            int delta = ASPIRATION_INITIAL_DELTA;  // Start with configured value (e.g., 18)
            int score = rootMoves[pvIdx].previousScore;

            // Use aspiration windows at higher depths
            if (rootDepth >= ASPIRATION_MIN_DEPTH && score != -VALUE_INFINITE) {
                alpha = std::max(score - delta, -VALUE_INFINITE);
                beta = std::min(score + delta, VALUE_INFINITE);
            }

            if (pvIdx > 0) {
                int prevScore = rootMoves[pvIdx - 1].score;
                beta = std::min(beta, prevScore + 1);
                alpha = std::min(alpha, prevScore - delta);
            }

            // Exponential aspiration window loop
            // Retry 1: +/- delta (e.g., 18)
            // Retry 2: +/- delta*3 (e.g., 54)
            // Retry 3: +/- delta*9 (e.g., 162)
            // Retry 4+: +/- INFINITE
            while (true) {
                score = search(board, alpha, beta, rootDepth, false);

                if (stopped) break;

                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

                if (score <= alpha) {
                    // Fail low - widen window exponentially (3x)
                    beta = (alpha + beta) / 2;
                    delta *= 3;  // Exponential widening

                    // If delta exceeds reasonable bounds, go full window
                    if (delta > 500) {
                        alpha = -VALUE_INFINITE;
                    } else {
                        alpha = std::max(score - delta, -VALUE_INFINITE);
                    }
                } else if (score >= beta) {
                    // Fail high - widen window exponentially (3x)
                    delta *= 3;  // Exponential widening

                    // If delta exceeds reasonable bounds, go full window
                    if (delta > 500) {
                        beta = VALUE_INFINITE;
                    } else {
                        beta = std::min(score + delta, VALUE_INFINITE);
                    }
                } else {
                    // Search completed within window
                    break;
                }
            }

            // Sort moves after completing this PV (keep previous PVs stable)
            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

            // [FIX] Detect and recover from suspicious search results
            // If PV is empty and score changed drastically (>500cp swing), this is likely
            // a search instability issue. Restore previous iteration's PV to prevent
            // tactical blindness where engine plays a bad move based on incomplete search.
            // NOTE: After stable_sort, we must find the move by its Move value since order changed
            if (!stopped && analyzedMove != MOVE_NONE) {
                // Find the move we just analyzed (it was at rootMoves[pvIdx] before sort,
                // now might have moved due to sorting)
                for (auto& rm : rootMoves) {
                    if (rm.move == analyzedMove) {
                        // Check for suspicious result: empty PV with huge score change
                        bool suspiciousResult = (rm.pv.length == 0) &&
                                                (previousIterScore != -VALUE_INFINITE) &&
                                                (std::abs(rm.score - previousIterScore) > 500);

                        // Also suspicious: PV becomes empty when previous had valid moves
                        bool pvDisappeared = (rm.pv.length == 0) && (previousPVLine.length > 0);

                        if (suspiciousResult || pvDisappeared) {
                            // Restore previous iteration's PV and score
                            rm.pv = previousPVLine;
                            rm.score = previousIterScore;
                        }
                        break;
                    }
                }
            }

            if (!stopped) {
                // Report this PV line
                RootMove& rm = rootMoves[pvIdx];

                // [FIX] Check if first TWO moves in PV are valid, not just length > 0
                // PV can have length > 0 but contain MOVE_NONE or illegal moves due to search instability
                // We check second move too because search instability often corrupts continuation moves
                bool pvValid = (rm.pv.length > 0) &&
                               (rm.pv.moves[0] != MOVE_NONE) &&
                               MoveGen::is_pseudo_legal(rootBoard, rm.pv.moves[0]) &&
                               MoveGen::is_legal(rootBoard, rm.pv.moves[0]);

                // If first move is valid but there should be more moves, validate second move too
                if (pvValid && rm.pv.length > 1) {
                    Move firstMove = rm.pv.moves[0];
                    Move secondMove = rm.pv.moves[1];

                    if (secondMove == MOVE_NONE) {
                        pvValid = false;
                    } else {
                        // Validate second move on the resulting position
                        Board tempBoard = rootBoard;
                        StateInfo si;
                        tempBoard.do_move(firstMove, si);

                        if (!MoveGen::is_pseudo_legal(tempBoard, secondMove) ||
                            !MoveGen::is_legal(tempBoard, secondMove)) {
                            pvValid = false;
                        }
                    }
                }

                // [FIX] If current PV is valid, update our best known PV for this specific move
                if (pvValid) {
                    uint16_t moveKey = rm.move.raw();
                    bestKnownPVPerMove[moveKey] = {rm.pv, rm.score};

                    // Also update overall best if this is better
                    if (rm.score > overallBestScore || overallBestMove == MOVE_NONE) {
                        overallBestMove = rm.move;
                        overallBestPV = rm.pv;
                        overallBestScore = rm.score;
                    }

                    report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                }
                // If current PV is invalid, try to restore from per-move cache
                else {
                    uint16_t moveKey = rm.move.raw();
                    auto it = bestKnownPVPerMove.find(moveKey);

                    if (it != bestKnownPVPerMove.end() && it->second.first.length > 0) {
                        // Found cached PV for this move - use it
                        rm.pv = it->second.first;
                        rm.score = it->second.second;
                        report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                    }
                    // Fall back to overall best PV
                    else if (overallBestPV.length > 0 && overallBestPV.moves[0] != MOVE_NONE) {
                        report_info(rootBoard, rootDepth, overallBestScore, overallBestPV, pvIdx + 1);
                    }
                    else {
                        // No valid PV available - report current result anyway
                        report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                    }
                }
            }
        }

        // After searching all PVs, update root best move from first PV
        if (!stopped && !rootMoves.empty()) {
            // [FIX] Check if search at this depth produced reliable results using proper validation
            // Not just pv.length > 0, but first move must also be valid
            bool searchReliable = (rootMoves[0].pv.length > 0) &&
                                  (rootMoves[0].pv.moves[0] != MOVE_NONE) &&
                                  MoveGen::is_pseudo_legal(board, rootMoves[0].pv.moves[0]) &&
                                  MoveGen::is_legal(board, rootMoves[0].pv.moves[0]);

            if (!searchReliable) {
                // Try to restore from backup first
                if (rootMovesBackup.size() > 0 && rootMovesBackup[0].pv.length > 0 &&
                    rootMovesBackup[0].pv.moves[0] != MOVE_NONE) {
                    rootMoves = rootMovesBackup;
                }
                // If backup also doesn't help, try to restore best move from per-move cache
                else if (overallBestMove != MOVE_NONE) {
                    // Find the overall best move in rootMoves and restore its PV
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
                // [PERBAIKAN] Check both pseudo-legal and legal to catch all edge cases
                if (MoveGen::is_pseudo_legal(board, candidate) && MoveGen::is_legal(board, candidate)) {
                    rootBestMove = candidate;
                }
            } else if (bestRM.move != MOVE_NONE &&
                       MoveGen::is_pseudo_legal(board, bestRM.move) &&
                       MoveGen::is_legal(board, bestRM.move)) {
                rootBestMove = bestRM.move;
            }
            // [FIX] Ultimate fallback: use overall best move if available
            else if (overallBestMove != MOVE_NONE &&
                     MoveGen::is_pseudo_legal(board, overallBestMove) &&
                     MoveGen::is_legal(board, overallBestMove)) {
                rootBestMove = overallBestMove;
            }

            rootPonderMove = MOVE_NONE;
            if (bestRM.pv.length > 1 && rootBestMove != MOVE_NONE) {
                Move ponderCandidate = bestRM.pv.moves[1];
                if (ponderCandidate != MOVE_NONE) {
                    // Validate ponder move by making best move first
                    StateInfo si;
                    Board tempBoard = board;  // Copy board
                    tempBoard.do_move(rootBestMove, si);

                    // Check if ponder move is legal in the new position
                    if (MoveGen::is_legal(tempBoard, ponderCandidate)) {
                        rootPonderMove = ponderCandidate;
                    }
                }
            }

            // Copy best PV to pvLines[0] for compatibility
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
                            bestMoveStability = 0;  // Reset on move change
                        }
                    }

                    // Fail-Low Time Extension
                    // When score is dropping (failing low), we need more time
                    bool failingLow = false;
                    if (previousRootScore != VALUE_NONE) {
                        int scoreDrop = previousRootScore - score;

                        if (scoreDrop > 30) {  // Score dropped by 30+ centipawns
                            failLowCount++;
                            failingLow = true;
                            lastFailLowScore = score;

                            // Extend time proportionally to the drop
                            int extension = std::min(scoreDrop * 2, optimumTime / 2);
                            optimumTime = std::min(maximumTime, optimumTime + extension);
                        } else if (scoreDrop < -20) {
                            // Score improved significantly - reset fail-low
                            failLowCount = 0;
                        }

                        // Consecutive fail-lows need even more time
                        if (failLowCount >= 2) {
                            int panicExtension = optimumTime / 3;
                            optimumTime = std::min(maximumTime, optimumTime + panicExtension);
                        }
                    }

                    // Complexity-Based Time Allocation
                    // Estimate position complexity and adjust time accordingly
                    {
                        int complexity = 50;  // Base complexity

                        // Factor 1: Number of root moves (more options = more complex)
                        int numMoves = static_cast<int>(rootMoves.size());
                        if (numMoves > 35) complexity += 15;
                        else if (numMoves > 25) complexity += 10;
                        else if (numMoves < 10) complexity -= 15;
                        else if (numMoves < 15) complexity -= 10;

                        // Factor 2: Score variance in top moves (similar scores = harder choice)
                        if (rootMoves.size() >= 3) {
                            int topScore = rootMoves[0].score;
                            int thirdScore = rootMoves[2].score;
                            int spread = topScore - thirdScore;

                            if (spread < 20) complexity += 20;       // Very close scores
                            else if (spread < 50) complexity += 10;  // Moderately close
                            else if (spread > 200) complexity -= 15; // Clear best move
                        }

                        // Factor 3: Absolute score (winning/losing positions might be simpler)
                        if (std::abs(score) > 300) complexity -= 10;
                        if (std::abs(score) > 600) complexity -= 10;

                        // Factor 4: Best move stability contributes to simplicity
                        complexity -= bestMoveStability * 3;

                        // Clamp complexity
                        positionComplexity = std::clamp(complexity, 10, 100);

                        // Adjust optimum time based on complexity
                        // complexity 50 = normal, <50 = use less time, >50 = use more time
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

                    // Check for big score change (anti-blunder safety)
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

                    // Save current PV for next iteration
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
                    bool scoreStable = scoreFluctuation < 30;  // Less than 30cp swing

                    // =========================================================================
                    // SIGNAL 4: Calculate Stability Factor
                    // =========================================================================
                    double stabilityFactor = 1.0 - (bestMoveStability * 0.05);  // Up to 50% reduction
                    stabilityFactor = std::max(stabilityFactor, 0.5);
                    int effectiveOptimum = static_cast<int>(optimumTime * stabilityFactor);

                    // =========================================================================
                    // MINIMUM DEPTH CHECK
                    // =========================================================================
                    const int MIN_DEPTH_FOR_EARLY_STOP = 10;  // Reduced from 12 for quicker response

                    // =========================================================================
                    // COMBINED SIGNAL EARLY STOP DECISION
                    // =========================================================================
                    if (multiPV == 1) {
                        bool isMateScore = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;

                        if (!isMateScore && rootDepth >= MIN_DEPTH_FOR_EARLY_STOP) {

                            // Anti-blunder safety: don't stop if big score change in last 3 depths
                            bool recentBigChange = (rootDepth - lastBigScoreChange) <= 3;

                            if (!recentBigChange) {
                                // CONDITION A: Very stable - best move same for 5+ iterations AND PV stable
                                if (bestMoveStability >= 5 && pvStability >= 3 && scoreStable &&
                                    elapsed > effectiveOptimum * 0.6) {
                                    break;
                                }

                                // CONDITION B: Stable - best move same for 4+ iterations
                                if (bestMoveStability >= 4 && scoreStable &&
                                    elapsed > effectiveOptimum * 0.7) {
                                    break;
                                }

                                // CONDITION C: Normal - past optimum time, not failing low
                                if (!failingLow && elapsed > effectiveOptimum * 0.85) {
                                    break;
                                }

                                // CONDITION D: Failing low but past hard limit
                                if (failingLow && elapsed > optimumTime * 0.95) {
                                    break;
                                }
                            }
                        } else if (isMateScore) {
                            // Mate handling: can stop early if mate is definite
                            int mateIn = (score > 0) ?
                                (VALUE_MATE - score + 1) / 2 :
                                std::abs((VALUE_MATE + score) / 2);

                            if (rootDepth >= MIN_DEPTH_FOR_EARLY_STOP &&
                                mateIn <= 4 && bestMoveStability >= 3 &&
                                elapsed > optimumTime * 0.7) {
                                break;  // Mate in 1-4 is definitive
                            }
                        }
                    }
                }

                previousRootScore = score;
                previousRootBestMove = rootBestMove;
            }
            // Emergency mode: use minimal time, check more frequently
            else if (emergencyMode) {
                auto now = std::chrono::steady_clock::now();
                int elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                );

                // In emergency mode, stop as soon as we have a reasonable move
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
    const bool pvNode = (beta - alpha) > 1;
    // rootNode removed - was unused (warning fix)

    // Calculate relative ply from root position (not absolute game ply)
    int ply = board.game_ply() - rootPly;

    if (ply >= MAX_PLY) {
        return evaluate(board);
    }
    // Update selective depth
    if (ply > searchStats.selDepth) {
        searchStats.selDepth = ply;
    }

    // Prefetch TT entry for the current position early
    TT.prefetch(board.key());

    // Check for time/node limits - check frequently for responsiveness
    // Using 1023 (1024-1) for fast time controls
    if ((searchStats.nodes & 4095) == 0) {
        check_time();
    }

    if (stopped) return 0;

    pvLines[ply].clear();

    // Get stack pointer for current ply
    // ply is guaranteed < MAX_PLY here
    SearchStack* ss = &stack[ply + 2];

    // Quiescence search at depth 0
    // Start qsearch with depth 0 (standard) - higher values hurt NPS significantly
    // Tactical depth is achieved through proper qsearch extensions for checks
    if (depth <= 0) {
        return qsearch(board, alpha, beta, 0);
    }

    ++searchStats.nodes;

    // Draw detection: check for repetition, 50-move rule, insufficient material
    // Skip at root node (ply 0) to ensure we return a move
    if (ply > 0 && board.is_draw(ply)) {
        // Apply contempt factor to draws
        // Positive contempt = we dislike draws (return slightly negative)
        // Negative contempt = we like draws (return slightly positive)
        int contempt = get_contempt(board);
        // Negate for side to move: if we're winning with positive contempt,
        // a draw should score worse (negative from our perspective)
        return -contempt;
    }

    // Mate distance pruning
    alpha = std::max(alpha, -VALUE_MATE + ply);
    beta = std::min(beta, VALUE_MATE - ply - 1);
    if (alpha >= beta) {
        return alpha;
    }

    // Also clear child's PV to prevent stale moves from previous searches
    if (ply + 1 < MAX_PLY) {
        pvLines[ply + 1].clear();
    }

    // Transposition table probe
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    // Primary Move for heuristics (take first available)
    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;
    if (!ttHit && ttMove != MOVE_NONE) {
        // If probe says no hit (e.g. empty), but get_moves found something(?), trust probe slightly less or use move anyway.
        // Actually probe returns entry even if not found.
        // But get_moves only returns matches.
        // So trust get_moves for the move.
    }

    // Validate valid moves to avoid illegal moves from hash collisions
    if (ttMove != MOVE_NONE && (!MoveGen::is_pseudo_legal(board, ttMove) || !MoveGen::is_legal(board, ttMove))) {
        ttMove = MOVE_NONE;
        ttHit = false; // Disable cutoff if primary move is corrupt
        // Clear corrupt move from list so MovePicker doesn't try it
        if (ttMoveCount > 0) ttMoves[0] = MOVE_NONE;
    }
    int ttScore = ttHit ? score_from_tt(tte->score(), ply) : VALUE_NONE;
    int ttDepth = ttHit ? tte->depth() : 0;
    Bound ttBound = ttHit ? tte->bound() : BOUND_NONE;

    // [PERBAIKAN] Validasi tambahan untuk mate scores dari TT
    // Mate score dari hash collision sangat berbahaya - bisa return evaluasi salah
    // Jika ttMove invalid TAPI ttScore adalah mate score, jangan percaya entry ini
    if (ttHit && ttMove == MOVE_NONE && std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY) {
        // Mate score tanpa move yang valid sangat mencurigakan
        ttHit = false;
        ttScore = VALUE_NONE;
    }

    // TT cutoff (only in non-PV nodes)
    // [PERBAIKAN] Skip TT cutoff untuk mate scores yang ekstrem tanpa validasi
    // Ini mencegah false mate dari hash collision
    bool ttMateScore = std::abs(ttScore) >= VALUE_MATE_IN_MAX_PLY;
    bool allowTTCutoff = !pvNode && ttHit && ttDepth >= depth;

    // Untuk mate score, tambahan validasi ketat
    if (allowTTCutoff && ttMateScore) {
        // [PERBAIKAN KRITIS] Validasi sangat ketat untuk mate scores
        // 1. TT move harus valid (tidak MOVE_NONE)
        // 2. Mate distance harus masuk akal relatif terhadap depth yang disimpan

        if (ttMove == MOVE_NONE) {
            // Mate tanpa move adalah sangat mencurigakan
            allowTTCutoff = false;
        } else {
            int mateDistance = std::abs(VALUE_MATE - std::abs(ttScore));

            // Untuk mate in N, kita butuh setidaknya depth >= N*2 untuk yakin
            // karena mate perlu verified dari kedua sisi
            if (ttDepth < mateDistance * 2) {
                allowTTCutoff = false;
            }

            // Jangan percaya mate claim yang kedalamannya tidak masuk akal
            // dengan ply saat ini - jika mateDistance sangat kecil tapi ply besar,
            // berarti ini mungkin dari posisi yang berbeda
            if (mateDistance < 1 || (ply > 0 && mateDistance > ply + depth)) {
                allowTTCutoff = false;
            }
        }
    }

    if (allowTTCutoff) {
        if ((ttBound == BOUND_EXACT) ||
            (ttBound == BOUND_LOWER && ttScore >= beta) ||
            (ttBound == BOUND_UPPER && ttScore <= alpha)) {
            // [PERBAIKAN] Update PV dengan ttMove agar parent mendapat PV yang lengkap
            // Tanpa ini, PV line akan terpotong saat TT cutoff
            if (ttMove != MOVE_NONE && ply < MAX_PLY) {
                pvLines[ply].length = 1;
                pvLines[ply].moves[0] = ttMove;
            }
            return ttScore;
        }
    }

    // Get static evaluation
    int staticEval;
    int correctedStaticEval;  // Static eval with correction history applied
    bool inCheck = board.in_check();
    Color us = board.side_to_move();

    if (inCheck) {
        staticEval = VALUE_NONE;
        correctedStaticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        staticEval = tte->eval();
        // Apply correction history to cached eval
        int correction = corrHistory.get(us, board.pawn_key());
        correctedStaticEval = staticEval + correction;
    } else {
        // Use lazy evaluation with alpha/beta for potential early cutoff
        staticEval = evaluate(board, alpha, beta);
        // Apply correction history to improve eval accuracy
        int correction = corrHistory.get(us, board.pawn_key());
        correctedStaticEval = staticEval + correction;
    }

    // Save evaluations to stack
    ss->staticEval = staticEval;
    ss->correctedStaticEval = correctedStaticEval;

    // Enhanced Improving Heuristic
    bool improving = false;
    bool improving4ply = false;
    int improvementDelta = 0;  // How much we improved (for LMR scaling)

    if (!inCheck && ss->staticEval != VALUE_NONE) {
        // 2-ply ago check (grandparent position)
        // ss is stack[ply+2], so 2-ply ago is stack[ply]
        if (ply >= 2 && stack[ply].staticEval != VALUE_NONE) {
            if (ss->staticEval >= stack[ply].staticEval) {
                improving = true;
                improvementDelta = ss->staticEval - stack[ply].staticEval;
            }
        } else {
            // If 2-ply ago was in check or unavailable, assume improving
            improving = true;
        }

        // 4-ply ago check (great-grandparent's grandparent)
        // stack[ply] is 2-ply ago, so stack[ply-2] is 4-ply ago
        if (ply >= 4 && stack[ply - 2].staticEval != VALUE_NONE) {
            if (ss->staticEval >= stack[ply - 2].staticEval) {
                improving4ply = true;
            }
        }
    }

    // If in check, assume improving to avoid over-pruning
    if (inCheck) improving = true;

    // Razoring (at low depths, if eval is far below alpha)
    // Modern engine pattern: use "predicted depth" - estimating depth after LMR
    // This makes razoring more stable as it reflects actual expected search depth
    if (!pvNode && !inCheck && depth <= 3 && depth >= 1) {
        // Estimate predicted depth: expect average LMR of ~1 for late moves
        // For depth 1-3, most moves will get reduced, so effective depth is lower
        int predictedDepth = std::max(1, depth - 1);

        // Use predicted depth for margin calculation
        int razorMarg = razoring_margin(predictedDepth);

        // Relax margin if position is improving (less aggressive pruning)
        if (improving) razorMarg += 50;

        if (correctedStaticEval + razorMarg <= alpha) {
            int razorScore = qsearch(board, alpha - razorMarg, alpha - razorMarg + 1);
            if (razorScore <= alpha - razorMarg) {
                return razorScore;
            }
        }
    }

    // Reverse futility pruning / Static null move pruning
    // Using dynamic margin based on depth and improving status (Stockfish-style)
    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMarg = rfp_margin(depth, improving);  // Dynamic function includes improving

        if (correctedStaticEval - rfpMarg >= beta && correctedStaticEval < VALUE_MATE_IN_MAX_PLY) {
            return correctedStaticEval;
        }
    }

    // Check if we have enough non-pawn material to do null move safely
    // This helps avoid zugzwang issues in pawn endgames
    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    bool doubleNullMove = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].nullMovePruned);

    // Mate Threat Detection - set to true if null move shows opponent can mate us
    bool mateThreat = false;

    // Null move pruning
    if (!pvNode && !inCheck && correctedStaticEval >= beta && depth >= 3 &&
        hasNonPawnMaterial && !doubleNullMove) {

        // Dynamic reduction based on depth and corrected eval
        int R = 3 + depth / 4 + std::min(3, (correctedStaticEval - beta) / 200);

        // Reduce more if not improving (likely to fail low anyway)
        if (!improving) R++;

        // Make null move
        StateInfo si;
        board.do_null_move(si);

        ss->nullMovePruned = true;

        int nullScore = -search(board, -beta, -beta + 1, depth - R - 1, !cutNode);

        board.undo_null_move();

        ss->nullMovePruned = false;

        if (stopped) return 0;

        if (nullScore >= beta) {
            // Don't trust mate scores from null move
            if (nullScore >= VALUE_MATE_IN_MAX_PLY) {
                nullScore = beta;
            }

            // Verification search at high depths to avoid zugzwang
            // If depth is high, do a reduced search to verify the null move result
            if (depth >= NULL_MOVE_VERIFY_DEPTH) {
                int verifyScore = search(board, beta - 1, beta, depth - R - 1, false);
                if (verifyScore >= beta) {
                    return nullScore;
                }
            } else {
                return nullScore;
            }
        }
        // MATE THREAT EXTENSION DETECTION
        // If null move shows we are getting mated (opponent has unstoppable mate threat),
        // set flag to extend search for defensive moves
        else if (depth >= MATE_THREAT_EXT_MIN_DEPTH && nullScore <= VALUE_MATED_IN_MAX_PLY) {
            mateThreat = true;
        }
    }

    // Multi-Cut Pruning
    // Use MovePicker with TT move for proper ordering - first moves are most likely to cutoff
    if (!pvNode && !inCheck && depth >= MULTI_CUT_DEPTH && cutNode && ttMove != MOVE_NONE) {
        int multiCutCount = 0;
        int movesTried = 0;

        // Use MovePicker for ordered moves (TT move first, then good captures, etc.)
        MovePicker mcPicker(board, ttMoves, ttMoveCount, ply, killers, counterMoves, history, previousMove,
                            nullptr, nullptr, &captureHist);
        Move m;

        while ((m = mcPicker.next_move()) != MOVE_NONE && movesTried < MULTI_CUT_COUNT + 2) {
            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            ++movesTried;

            // Do a shallow null-window search
            StateInfo si;
            board.do_move(m, si);

            int mcDepth = depth - 1 - MULTI_CUT_DEPTH / 2;  // Reduced depth
            int mcScore = -search(board, -beta, -beta + 1, mcDepth, !cutNode);

            board.undo_move(m);

            if (stopped) return 0;

            // Count beta cutoffs
            if (mcScore >= beta) {
                ++multiCutCount;

                // If enough moves caused cutoffs, prune this node
                if (multiCutCount >= MULTI_CUT_REQUIRED) {
                    return beta;
                }
            }

            // Stop if we've tried enough moves
            if (movesTried >= MULTI_CUT_COUNT) {
                break;
            }
        }
    }

    // ProbCut (Probabilistic Cutoff)
    if (!pvNode && !inCheck && depth >= PROBCUT_DEPTH &&
        std::abs(beta) < VALUE_MATE_IN_MAX_PLY) {

        int probCutBeta = beta + PROBCUT_MARGIN;
        int probCutDepth = depth - 4;  // Shallow search

        // Generate captures for ProbCut
        MoveList captures;
        MoveGen::generate_captures(board, captures);

        for (size_t i = 0; i < captures.size(); ++i) {
            Move m = captures[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            // Only consider good captures (positive SEE)
            if (!SEE::see_ge(board, m, 0)) {
                continue;
            }

            StateInfo si;
            board.do_move(m, si);

            // First do a qsearch to get a quick estimate
            int qScore = -qsearch(board, -probCutBeta, -probCutBeta + 1, 0);

            // If qsearch looks promising, do a proper shallow search
            if (qScore >= probCutBeta) {
                int probCutScore = -search(board, -probCutBeta, -probCutBeta + 1,
                                           probCutDepth, !cutNode);

                board.undo_move(m);

                if (stopped) return 0;

                // If shallow search confirms, we can prune
                if (probCutScore >= probCutBeta) {
                    return probCutScore;
                }
            } else {
                board.undo_move(m);
            }

            if (stopped) return 0;
        }
    }

    // Internal Iterative Reductions (IIR) - Replaces IID
    // Use separate variable to not corrupt depth for TT store and extensions
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

    // One Reply Extension - detect if there's only one legal move
    // When there's only one move, we should extend since it's forced
    // Only check when in check (most common forced move scenario) for efficiency
    bool oneReplyExtension = false;
    if (inCheck && depth >= ONE_REPLY_EXT_MIN_DEPTH) {
        // Count legal moves quickly by generating evasions
        MoveList evasions;
        MoveGen::generate_all(board, evasions);
        int legalCount = 0;
        for (size_t i = 0; i < evasions.size() && legalCount <= 1; ++i) {
            if (MoveGen::is_legal(board, evasions[i].move)) {
                legalCount++;
            }
        }
        if (legalCount == 1) {
            oneReplyExtension = true;
        }
    }

    // Move loop
    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

    // Track captures searched for failed-move penalty
    Move capturesSearched[32];
    Piece capturePieces[32];      // The piece that moved for each capture
    PieceType capturedTypes[32];  // The captured piece type for each capture
    int captureSEE[32];           // SEE value for each capture (for penalty scaling)
    int captureCount = 0;

    // Track singularity for best move check
    bool singularSearched = false;

    // Root node flag for MultiPV handling
    const bool rootNode = (ply == 0);

    // Get continuation history entries for move ordering
    const ContinuationHistoryEntry* contHist1ply = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].contHistory) ?
                                                    stack[ply + 1].contHistory : nullptr;
    const ContinuationHistoryEntry* contHist2ply = (ply >= 2 && ply < MAX_PLY + 4 && stack[ply].contHistory) ?
                                                    stack[ply].contHistory : nullptr;

    MovePicker mp(board, ttMoves, ttMoveCount, ply, killers, counterMoves, history, previousMove,
                  contHist1ply, contHist2ply, &captureHist);

    size_t rootMoveIdx = 0;
    Move m;

    // Main move loop
    while (true) {
        // Get next move based on whether we're at root or not
        if (rootNode) {
            // At root, iterate through rootMoves starting from pvIdx
            if (rootMoveIdx + pvIdx >= rootMoves.size()) {
                break;  // No more moves
            }
            m = rootMoves[rootMoveIdx + pvIdx].move;
            ++rootMoveIdx;
        } else {
            m = mp.next_move();
            if (m == MOVE_NONE) {
                break;  // No more moves
            }
        }

        // Skip excluded move (used for singular extension verification)
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

        // Get piece info for passed pawn detection
        Piece movedPiece = board.piece_on(m.from());
        PieceType movedPt = type_of(movedPiece);
        Color us = board.side_to_move();

        // LAZY Threat Detection - only compute when needed for pruning
        // These are expensive, so only calculate for late moves that might be pruned
        bool createsThreat = false;
        bool createsFork = false;
        bool escapesAttack = false;
        bool threatDetectionDone = false;  // Flag to avoid recalculating

        // Lambda to lazily compute threat detection
        auto computeThreatDetection = [&]() {
            if (threatDetectionDone) return;
            threatDetectionDone = true;

            // Check if the piece we're moving is under attack (escaping danger)
            Bitboard attackersToFrom = board.attackers_to(m.from(), board.pieces()) & board.pieces(~us);
            if (attackersToFrom) {
                int pieceValueMoving = PieceValue[movedPt];

                // Find the least valuable attacker
                int minAttackerValue = 20000;
                for (Bitboard atk = attackersToFrom; atk; ) {
                    Square atkSq = pop_lsb(atk);
                    PieceType atkPt = type_of(board.piece_on(atkSq));
                    minAttackerValue = std::min(minAttackerValue, PieceValue[atkPt]);
                }

                // Queen or rook under attack is always escape-worthy
                if (movedPt == QUEEN || movedPt == ROOK || pieceValueMoving >= minAttackerValue) {
                    // Simplified escape check - just mark it, don't do full destination analysis
                    escapesAttack = true;
                }
            }

            // Only compute threat creation for quiet moves
            if (!isCapture && !givesCheck) {
                Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
                Bitboard attacksAfter = attacks_bb(movedPt, m.to(), newOccupied);

                // Check for threats to queen/rook
                Bitboard valuableEnemies = board.pieces(~us, QUEEN) | board.pieces(~us, ROOK);
                if (attacksAfter & valuableEnemies) {
                    createsThreat = true;
                }

                // Check for forks - simplified (only knights/bishops can really fork effectively)
                if (movedPt == KNIGHT || movedPt == BISHOP) {
                    Bitboard allMinorsAndUp = valuableEnemies | board.pieces(~us, BISHOP) | board.pieces(~us, KNIGHT);
                    if (popcount(attacksAfter & allMinorsAndUp) >= 2) {
                        createsFork = true;
                        createsThreat = true;
                    }
                }

                // [TACTICAL BLINDNESS FIX] Detect potential mate threats
                // Queen/Rook moves that attack squares adjacent to enemy king are dangerous
                // This catches moves like Qg6 that threaten Qg7# or Qh7#
                if (movedPt == QUEEN || movedPt == ROOK) {
                    Square enemyKingSq = board.king_square(~us);
                    Bitboard kingZone = king_attacks_bb(enemyKingSq);

                    // If we attack squares around enemy king, this could be a mate threat
                    if (attacksAfter & kingZone) {
                        createsThreat = true;
                    }

                    // Also check if we directly attack the king square itself
                    if (attacksAfter & square_bb(enemyKingSq)) {
                        createsThreat = true;
                    }
                }
            }
        };

        // Skip late quiet moves at low depths when we're not improving
        // LAZY: Only compute threats if we're past LMP threshold
        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            !givesCheck && bestScore > VALUE_MATED_IN_MAX_PLY) {
            int lmpThresh = lmp_threshold(depth, improving);
            if (moveCount > lmpThresh) {
                // Only now compute threat detection
                computeThreatDetection();
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // Futility pruning - Don't prune moves that escape attacks on valuable pieces
        // LAZY: compute threat detection only when needed
        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck) {
            int futilMarg = futility_margin(depth, improving);
            if (correctedStaticEval + futilMarg <= alpha) {
                computeThreatDetection();  // Lazy compute
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // SEE pruning for captures
        // Skip losing captures at low depths
        // [FIX] NEVER prune captures that give check - they can be mating sacrifices!
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

        // SEE pruning for quiet moves (prune if quiet move has very negative SEE)
        // LAZY: compute threat detection only when needed
        if (!pvNode && !inCheck && depth <= 3 && !isCapture && !givesCheck) {
            int seeThreshold = improving ?
                -SEE_QUIET_IMPROVING_FACTOR * depth :
                -SEE_QUIET_NOT_IMPROVING_FACTOR * depth;
            if (!SEE::see_ge(board, m, seeThreshold)) {
                computeThreatDetection();  // Lazy compute
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // History Leaf Pruning - LAZY
        if (!pvNode && !inCheck && depth <= HISTORY_LEAF_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY) {
            int histScore = history.get(board.side_to_move(), m);
            if (histScore < -HISTORY_LEAF_PRUNING_MARGIN * depth) {
                computeThreatDetection();  // Lazy compute
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // =====================================================================
        // COUNTERMOVE HISTORY PRUNING - LAZY
        // =====================================================================
        if (!pvNode && !inCheck && depth <= COUNTER_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY && contHist1ply) {

            PieceType pt = type_of(movedPiece);
            int cmHistScore = contHist1ply->get(pt, m.to());

            if (cmHistScore < -COUNTER_HIST_PRUNING_MARGIN * depth) {
                computeThreatDetection();  // Lazy compute
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // =====================================================================
        // FOLLOW-UP HISTORY PRUNING - LAZY
        // =====================================================================
        if (!pvNode && !inCheck && depth <= FOLLOWUP_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck &&
            bestScore > VALUE_MATED_IN_MAX_PLY && ply >= 4 && stack[ply - 2].contHistory) {

            const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
            PieceType pt = type_of(movedPiece);
            int followupScore = contHist4ply->get(pt, m.to());

            if (followupScore < -FOLLOWUP_HIST_PRUNING_MARGIN * depth) {
                computeThreatDetection();  // Lazy compute
                if (!createsThreat && !escapesAttack) {
                    continue;
                }
            }
        }

        // Extensions

        int extension = 0;
        int currentExtensions = (ss->ply >= 2 && ply >= 2) ? stack[ply + 1].extensions : 0;

        // Double extension counter - track how many times we've double-extended
        int doubleExtensions = (ply >= 1) ? stack[ply + 1].doubleExtensions : 0;

        // Check extension (limited to MAX_EXTENSIONS total in path)
        // [PERBAIKAN] Safe checks (SEE >= 0) SELALU dipanjangkan untuk menemukan mat paksa
        if (givesCheck && currentExtensions < MAX_EXTENSIONS) {
            // Safe check = skak yang tidak langsung kalah material
            if (SEE::see_ge(board, m, 0)) {
                extension = 1;  // WAJIB extend untuk safe checks
            }
        }

        // In-check extension: when WE are in check, extend to find defensive resources
        if (inCheck && currentExtensions < MAX_EXTENSIONS) {
            extension = std::max(extension, 1);
        }

        // One Reply Extension: when there's only one legal move in check, extend deeper
        // This is a forced move so we want to analyze the consequences more thoroughly
        if (oneReplyExtension && currentExtensions < MAX_EXTENSIONS) {
            extension = std::max(extension, 1);
        }

        // Passed pawn extension (for pawn moves to 7th rank)
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
            // Extend to find defensive moves against the mate threat
            // Only extend if this move might be a defensive resource:
            // - Moves that give check (counter-attack)
            // - Moves that block or escape
            // We extend all moves here since any move could be the defense
            extension = 1;
        }

        // =====================================================================
        // PV EXTENSION
        // =====================================================================
        // In PV nodes, extend the first move to ensure the principal variation
        // is searched more deeply. This helps avoid horizon effects on critical lines.
        // Only apply at sufficient depth to avoid search explosion.
        if (pvNode && moveCount == 1 && depth >= PV_EXT_MIN_DEPTH &&
            currentExtensions < MAX_EXTENSIONS && extension == 0) {
            extension = 1;
        }

        // =====================================================================
        // CAPTURE EXTENSION - Extend important tactical captures
        // =====================================================================
        if (isCapture && currentExtensions < MAX_EXTENSIONS && !m.is_enpassant()) {
            Piece captured = board.piece_on(m.to());
            PieceType capturedPt = type_of(captured);

            // 1. Queen capture extension - capturing queen is almost always critical
            if (capturedPt == QUEEN && depth >= CAPTURE_EXT_MIN_DEPTH) {
                if (SEE::see_ge(board, m, 0)) {
                    extension = std::max(extension, 1);
                }
            }

            // 2. Rook trade extension - trading rooks changes game character
            if (capturedPt == ROOK && movedPt == ROOK && depth >= CAPTURE_EXT_MIN_DEPTH) {
                if (SEE::see_ge(board, m, 0)) {
                    extension = std::max(extension, 1);
                }
            }

            // 3. Recapture extension - extend when recapturing on the same square
            if (previousMove != MOVE_NONE && m.to() == previousMove.to()) {
                if (SEE::see_ge(board, m, 0)) {
                    extension = std::max(extension, 1);
                }
            }

            // 4. High-value capture with good SEE (winning significant material)
            if (capturedPt == ROOK || capturedPt == QUEEN) {
                if (SEE::see_ge(board, m, CAPTURE_EXT_SEE_THRESHOLD)) {
                    extension = std::max(extension, 1);
                }
            }
        }

        // =====================================================================
        // KING ATTACK EXTENSION - Extend Queen/Rook moves attacking king zone
        // =====================================================================
        // [TACTICAL BLINDNESS FIX] Helps find mates like Qg6 threatening Qg7#
        if (!isCapture && !givesCheck && currentExtensions < MAX_EXTENSIONS &&
            extension == 0 && depth >= 4 && (movedPt == QUEEN || movedPt == ROOK)) {

            Square enemyKingSq = board.king_square(~us);
            Bitboard kingZone = king_attacks_bb(enemyKingSq);
            Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
            Bitboard attacksAfter = attacks_bb(movedPt, m.to(), newOccupied);

            // Extend if the move attacks squares adjacent to enemy king
            // This is a potential mate threat that deserves deeper search
            if (attacksAfter & (kingZone | square_bb(enemyKingSq))) {
                // Only extend if there's limited material (mostly for mating attacks)
                // Don't extend too much in complex positions
                int enemyMaterial = popcount(board.pieces(~us)) - 2; // -2 for king+pawns baseline
                if (enemyMaterial <= 4) {  // Few pieces around king = mating attack likely
                    extension = 1;
                }
            }
        }

        // =====================================================================
        // SINGULAR EXTENSION - Enhanced with dynamic margins
        // =====================================================================
        // If the TT move is singular (much better than alternatives), extend it
        if (!singularSearched && depth >= SINGULAR_DEPTH && isTTMove &&
            ttHit && ttBound != BOUND_UPPER && ttDepth >= depth - 3 &&
            std::abs(ttScore) < VALUE_MATE_IN_MAX_PLY && currentExtensions < MAX_EXTENSIONS) {

            singularSearched = true;

            // Dynamic singular margin based on multiple factors:
            int singularMargin = SINGULAR_MARGIN;

            // 1. Wider margin if TT entry is from a shallower search (less reliable)
            int depthDiff = depth - ttDepth;
            if (depthDiff > 0) {
                singularMargin += depthDiff * SINGULAR_TT_DEPTH_PENALTY;
            }

            // 2. Tighter margin when NOT improving (be more aggressive with extensions)
            //    because we need to search more carefully in worsening positions
            if (!improving) {
                singularMargin -= SINGULAR_IMPROVING_BONUS;
            }

            // 3. Slightly tighter margin in PV nodes (important lines deserve more search)
            if (pvNode) {
                singularMargin -= 5;
            }

            int singularBeta = std::max(ttScore - singularMargin * depth / 8, -VALUE_MATE);
            int singularDepth = (depth - 1) / 2;

            ss->excludedMove = m;
            int singularScore = search(board, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = MOVE_NONE;

            if (singularScore < singularBeta) {
                // TT move is singular - extend it
                extension = 1;

                // Dynamic double extension threshold based on depth
                // Higher depths need larger margin to trigger double extension
                int doubleExtThreshold = SINGULAR_DOUBLE_EXT_BASE + (depth / 4) * 10;

                // Double extension: if singular margin is very large and within limit
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
        // LMR: Moves that give check are NOT reduced at all.
        // For other tactical moves (createsThreat), we compute lazily.
        if (depth >= 2 && moveCount > 1 && !isCapture && !isPromotion && !givesCheck) {
            // Compute threat detection lazily for LMR decision
            computeThreatDetection();

            // Skip LMR for threatening moves
            if (!createsThreat) {
                reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];

                if (cutNode) {
                    reduction += 1;
                }

                if (!improving) {
                    reduction += 1;
                }

                if (moveCount > 15) {
                    reduction += 1;
                }

                if (pvNode) {
                    reduction -= 1;
                }

                if (inCheck) {
                    reduction -= 1;
                }

                if (createsFork) {
                    reduction -= 1;
                }

                if (escapesAttack) {
                    reduction -= 1;  // Reduce less for escape moves
                }

                if (killers.is_killer(ply, m) ||
                    (previousMove && m == counterMoves.get(board.piece_on(previousMove.to()), previousMove.to()))) {
                    reduction -= 2;
                }

                if (isTTMove) {
                    reduction -= 1;
                }

                if (improvementDelta > 0) {
                    reduction -= std::min(improvementDelta / 50, 2);
                }

                int histScore = history.get(board.side_to_move(), m);

                PieceType pt = type_of(movedPiece);
                if (contHist1ply) {
                    histScore += CONT_HIST_1PLY_WEIGHT * contHist1ply->get(pt, m.to());
                }
                if (contHist2ply) {
                    histScore += CONT_HIST_2PLY_WEIGHT * contHist2ply->get(pt, m.to());
                }
                if (ply >= 4 && stack[ply - 2].contHistory) {
                    const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
                    histScore += CONT_HIST_4PLY_WEIGHT * contHist4ply->get(pt, m.to());
                }

                reduction -= std::clamp(histScore / HISTORY_LMR_DIVISOR, -HISTORY_LMR_MAX_ADJ, HISTORY_LMR_MAX_ADJ);

                reduction = std::clamp(reduction, 0, newDepth - 1);
            }  // end if (!createsThreat)
        }  // end LMR block

        // Make the move
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

        // Principal Variation Search
        if (moveCount == 1) {
            // First move - search with full window
            score = -search(board, -beta, -alpha, newDepth, false);
        } else {
            // Null window search with reduction
            score = -search(board, -alpha - 1, -alpha, newDepth - reduction, true);

            // Re-search without reduction if score > alpha
            if (score > alpha && reduction > 0) {
                score = -search(board, -alpha - 1, -alpha, newDepth, !cutNode);
            }

            // Full re-search if in PV and score > alpha
            if (score > alpha && score < beta) {
                score = -search(board, -beta, -alpha, newDepth, false);
            }
        }

        // Undo the move
        board.undo_move(m);

        // Update subtree node count for root moves
        if (rootNode) {
            U64 nodesAfter = searchStats.nodes;
            U64 nodeDiff = nodesAfter - nodesBefore;

            // Find the current move in rootMoves and update its subtree node count
            // Note: Since we are iterating via MovePicker or loop, we need to find by move
            // Optimization: we could track index, but search loop structure makes it tricky.
            // Linear scan is fine for calculating root moves (typically < 50 legal moves)
            for (auto& rm : rootMoves) {
                if (rm.move == m) {
                    rm.subtreeNodes += nodeDiff;
                    break;
                }
            }
        }

        if (stopped) {
            return 0;
        }

        // Track quiet moves for history updates
        if (!isCapture && quietCount < 64) {
            quietsSearched[quietCount++] = m;
        }

        // Track captures for failed-move penalty (capture history update)
        if (isCapture && captureCount < 32) {
            capturesSearched[captureCount] = m;
            capturePieces[captureCount] = movedPiece;
            Piece captured = board.piece_on(m.to());
            capturedTypes[captureCount] = (captured != NO_PIECE) ? type_of(captured) :
                                          (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);
            // Calculate SEE for penalty scaling (bad captures get stronger penalty)
            captureSEE[captureCount] = SEE::evaluate(board, m);
            captureCount++;
        }

        // Update best score
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;

            if (score > alpha) {
                // Update PV - guard against overflow
                if (ply + 1 < MAX_PLY) {
                    pvLines[ply].update(m, pvLines[ply + 1]);
                }

                // At root node, update rootMoves for MultiPV
                if (rootNode) {
                    // Find this move in rootMoves and update it
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
                    // BETA CUTOFF - Big bonus for the winning move
                    // =========================================================

                    int bonus = depth * depth;

                    // Update killer moves (only for quiet moves)
                    if (!isCapture) {
                        killers.store(ply, m);

                        // Store as mate killer if this move led to checkmate
                        if (score >= VALUE_MATE_IN_MAX_PLY) {
                            mateKillers.store(ply, m);
                        }

                        // Update counter move
                        if (previousMove) {
                            counterMoves.store(board.piece_on(m.from()), m.to(), m);
                        }

                        // Update history with big bonus for cutoff move
                        history.update_quiet_stats(board.side_to_move(), m,
                                                   quietsSearched, quietCount - 1, depth);

                        // Update continuation history with bonus
                        PieceType pt = type_of(movedPiece);
                        if (contHist1ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), bonus);
                        }
                        if (contHist2ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(pt, m.to(), bonus);
                        }

                        // Penalty for other quiet moves that were tried (failed to cutoff)
                        for (int i = 0; i < quietCount - 1; ++i) {
                            if (quietsSearched[i] != m) {
                                Piece qpc = board.piece_on(quietsSearched[i].from());
                                PieceType qpt = type_of(qpc);
                                Square qto = quietsSearched[i].to();

                                // Penalty in butterfly history
                                history.update(board.side_to_move(), quietsSearched[i], depth, false);

                                if (contHist1ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -bonus);
                                }
                                if (contHist2ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -bonus);
                                }
                            }
                        }
                    } else {
                        // Capture caused cutoff - big bonus
                        Piece captured = board.piece_on(m.to());
                        PieceType capturedPt = (captured != NO_PIECE) ? type_of(captured) :
                                               (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);
                        if (capturedPt != NO_PIECE_TYPE) {
                            captureHist.update_capture_stats(movedPiece, m.to(), capturedPt, depth, true);
                        }
                    }

                    // Penalty for ALL captures that were tried but failed to cutoff
                    // SEE-negative captures get stronger penalty
                    for (int i = 0; i < captureCount; ++i) {
                        if (capturesSearched[i] != m && capturedTypes[i] != NO_PIECE_TYPE) {
                            // Base penalty
                            int penaltyDepth = depth;

                            // SEE-negative captures get double penalty (they were bad ideas)
                            if (captureSEE[i] < 0) {
                                penaltyDepth = depth * 2;
                            }

                            // Apply penalty to failed captures
                            captureHist.update_capture_stats(capturePieces[i],
                                                             capturesSearched[i].to(),
                                                             capturedTypes[i],
                                                             penaltyDepth, false);
                        }
                    }

                    break;  // Fail high
                }

                // =========================================================
                // IMPROVED ALPHA but no cutoff - Small bonus
                // =========================================================
                {
                    int smallBonus = depth;  // Smaller bonus for raising alpha

                    if (!isCapture) {
                        // Small bonus in continuation history for improving alpha
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
        int penalty = depth;  // Medium penalty for failed moves

        bool bestIsCapture = !board.empty(bestMove.to()) || bestMove.is_enpassant();

        // If best move is quiet, penalize quiets that were tried but didn't improve alpha
        // The best move already got its bonus (either at cutoff or when it improved alpha)
        if (!bestIsCapture) {

            for (int i = 0; i < quietCount; ++i) {
                // Skip the best move itself
                if (quietsSearched[i] == bestMove) continue;

                Piece qpc = board.piece_on(quietsSearched[i].from());
                PieceType qpt = type_of(qpc);
                Square qto = quietsSearched[i].to();

                // Only apply penalty if this move didn't raise alpha at all
                // (Moves that raised alpha already got small bonus above)
                if (contHist1ply) {
                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -penalty);
                }
                if (contHist2ply) {
                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -penalty / 2);
                }
            }
        }

        // Penalize captures that were searched but didn't become best
        // SEE-negative captures get stronger penalty
        for (int i = 0; i < captureCount; ++i) {
            if (capturesSearched[i] != bestMove && capturedTypes[i] != NO_PIECE_TYPE) {
                // Base penalty (smaller than cutoff penalty)
                int penaltyDepth = depth / 2;

                // SEE-negative captures get double penalty
                if (captureSEE[i] < 0) {
                    penaltyDepth = depth;  // Full penalty for bad captures
                }

                captureHist.update_capture_stats(capturePieces[i],
                                                 capturesSearched[i].to(),
                                                 capturedTypes[i],
                                                 penaltyDepth, false);
            }
        }
    }

    // No legal moves
    if (moveCount == 0) {
        if (inCheck) {
            return -VALUE_MATE + ply;  // Checkmate
        } else {
            return 0;  // Stalemate
        }
    }

    // Store in transposition table
    // [PERBAIKAN KRITIS] Jangan simpan ke TT jika search dihentikan
    // Saat search dihentikan, child nodes return 0 yang bisa mengkorrupt bestScore
    // dan menyebabkan false mate scores disimpan ke TT
    Bound bound = bestScore >= beta ? BOUND_LOWER :
                  bestScore > alpha ? BOUND_EXACT : BOUND_UPPER;

    if (!stopped && tte) {
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation());
    }

    // Update Correction History
    // When we have a reliable search result (exact bound or fail high/low),
    // update the correction history to improve future static eval accuracy
    // Guards:
    // 1. Not in check (static eval unreliable in check)
    // 2. Static eval was computed (not VALUE_NONE)
    // 3. Depth >= 3 (shallow searches are noisy)
    // 4. Best score is NOT a mate score (mate is tactical, not eval error)
    // 5. Bound is reliable (EXACT, or fail high/low with moves tried)
    if (!inCheck && staticEval != VALUE_NONE && depth >= 3 &&
        std::abs(bestScore) < VALUE_MATE_IN_MAX_PLY) {

        // Calculate the difference between search result and static eval
        // Positive diff means eval was too pessimistic, negative means too optimistic
        int diff = bestScore - staticEval;

        // Clamp diff to prevent extreme corrections (max 500cp adjustment)
        diff = std::clamp(diff, -500, 500);

        // Only update on reliable results (EXACT or fail-high)
        // BOUND_UPPER (fail-low) is not reliable for correction
        if (bound == BOUND_EXACT ||
            (bound == BOUND_LOWER && bestScore >= beta)) {
            corrHistory.update(us, board.pawn_key(), diff, depth);
        }
    }

    return bestScore;
}

// Quiescence Search
// recaptureSquare: square of previous capture (for recapture detection)
// Recaptures on the same square do NOT reduce qsDepth to ensure exchanges complete
int Search::qsearch(Board& board, int alpha, int beta, int qsDepth, Square recaptureSquare) {
    ++searchStats.nodes;

    // Check for time limits
    if ((searchStats.nodes & 4095) == 0) {
        check_time();
    }

    if (stopped) return 0;

    // Calculate relative ply from root position
    int ply = board.game_ply() - rootPly;

    if (ply >= MAX_PLY) {
        return evaluate(board); // atau return alpha/beta
    }

    // Clear PV line for this ply to prevent stale data from leaking
    // to parent nodes, which could cause illegal move output
    pvLines[ply].clear();

    bool inCheck = board.in_check();

    // Jika dalam skak, kita HARUS cek semua legal evasions untuk detect skakmat
    // Gunakan counter terpisah untuk legal moves saat dalam skak
    int legalMoveCount = 0;

    // Stand pat (hanya jika tidak dalam skak) - use lazy eval
    int staticEval = inCheck ? -VALUE_INFINITE : evaluate(board, alpha, beta);

    if (!inCheck) {
        if (staticEval >= beta) {
            return staticEval;
        }
        if (staticEval > alpha) {
            alpha = staticEval;
        }
    }

    // Generate moves based on state and qsDepth
    MoveList moves;
    MoveList quietChecks;  // Separate list for quiet checking moves

    if (inCheck) {
        MoveGen::generate_evasions(board, moves);
    } else {
        MoveGen::generate_captures(board, moves);

        // Generate quiet checks at appropriate qsDepth
        // [OPTIMIZED] Use generate_checking_moves() instead of generate_quiets() + filter
        // This is significantly faster as it directly generates only moves that give check
        if (qsDepth >= QSEARCH_CHECK_DEPTH && qsDepth >= 0) {
            MoveGen::generate_checking_moves(board, quietChecks);
        }
    }

    // TT probe for move ordering
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    Move ttMoves[3];
    int ttMoveCount = 0;
    TT.get_moves(board.key(), ttMoves, ttMoveCount);

    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;

    // Validasi ttMove di qsearch dengan is_pseudo_legal dan is_legal
    if (ttMove != MOVE_NONE && (!MoveGen::is_pseudo_legal(board, ttMove) || !MoveGen::is_legal(board, ttMove))) {
        ttMove = MOVE_NONE;
        ttHit = false;
        if (ttMoveCount > 0) ttMoves[0] = MOVE_NONE;
    }

    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;
    Move m;

    // [CRITICAL FIX] When in check, we MUST iterate directly over evasion moves.
    // MovePicker for qsearch uses generate_captures() internally, which doesn't
    // generate evasion moves. This was causing incorrect checkmate detection
    // because legalMoveCount was always 0 when in check.
    if (inCheck) {
        // Search ALL evasion moves (no pruning when in check)
        for (size_t i = 0; i < moves.size(); ++i) {
            m = moves[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            ++legalMoveCount;  // Count legal evasions for checkmate detection

            // Make move
            StateInfo si;
            board.do_move(m, si);

            // For evasions, we don't track recapture squares (pass SQ_NONE)
            // and keep qsDepth unchanged to ensure we search all evasion lines

            int score = -qsearch(board, -beta, -alpha, qsDepth, SQ_NONE);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;  // Beta cutoff
                    }
                    alpha = score;
                }
            }
        }
    } else {
        // Not in check: use MovePicker for captures with proper ordering
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

            // [FIX] Check if this capture gives check BEFORE any pruning
            // Sacrifices that give check (e.g., Nxf7+, Qxh7+) must NOT be pruned
            bool captureGivesCheck = MoveGen::gives_check(board, m);

            // Delta pruning: skip captures that can't possibly raise alpha
            // NEVER prune captures that give check - they can lead to mate
            if (!m.is_promotion() && !captureGivesCheck) {
                // Conservative delta margin from search_constants.hpp
                // Never prune captures of major pieces (Queen, Rook)
                if (capturedPt != QUEEN && capturedPt != ROOK) {
                    if (staticEval + captureValue + DELTA_PRUNING_MARGIN < alpha) {
                        continue;  // Can't raise alpha
                    }
                }
            }

            // SEE pruning for losing captures
            // [FIX] NEVER prune captures that give check - they can be tactical sacrifices
            if (capturedPt != QUEEN && !captureGivesCheck) {
                // Only prune losing captures that DON'T give check
                if (!SEE::see_ge(board, m, -1)) {
                    continue;
                }
            }

            // Make move
            StateInfo si;
            board.do_move(m, si);

            // Determine if this is a recapture (capture on the same square as previous)
            // Recaptures should NOT reduce qsDepth to ensure exchange sequences complete
            bool isRecapture = (recaptureSquare != SQ_NONE && m.to() == recaptureSquare);
            bool isCapture = (capturedPt != NO_PIECE_TYPE) || m.is_enpassant();

            // Calculate new qsDepth:
            // - Recaptures: don't reduce (continue exchange sequence)
            // - Other captures: reduce by 1
            int newQsDepth = isRecapture ? qsDepth : qsDepth - 1;

            // Pass the capture target square for recapture detection in child
            Square newRecaptureSquare = isCapture ? m.to() : SQ_NONE;

            int score = -qsearch(board, -beta, -alpha, newQsDepth, newRecaptureSquare);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;  // Beta cutoff
                    }
                    alpha = score;
                }
            }
        }
    }

    // Then search quiet checks if any (hanya jika TIDAK dalam skak)
    if (!inCheck) {
        for (size_t i = 0; i < quietChecks.size(); ++i) {
            m = quietChecks[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            // SEE pruning for quiet checks
            if (!SEE::see_ge(board, m, 0)) {
                continue;  // Losing move
            }

            // Make move
            StateInfo si;
            board.do_move(m, si);

            // Search with reduced qsDepth since this is a quiet check
            // Quiet checks don't capture, so no recapture square
            int score = -qsearch(board, -beta, -alpha, qsDepth - 1, SQ_NONE);

            board.undo_move(m);

            if (stopped) return 0;

            if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (score >= beta) {
                        return score;  // Beta cutoff
                    }
                    alpha = score;
                }
            }
        }
    }

    // Checkmate detection - menggunakan legalMoveCount yang benar
    if (inCheck && legalMoveCount == 0) {
        return -VALUE_MATE + ply;
    }

    return bestScore;
}

// Static Evaluation - Using Advanced HCE from eval.hpp
int Search::evaluate(const Board& board) {
    // Check for known drawn endgames
    if (Tablebase::EndgameRules::is_known_draw(board)) {
        return 0;
    }

    int score = Eval::evaluate(board);

    // Apply scale factor for drawish endgames
    int scaleFactor = Tablebase::EndgameRules::scale_factor(board);
    if (scaleFactor != 128) {
        score = score * scaleFactor / 128;
    }

    return score;
}

// Overload with alpha/beta for lazy evaluation
int Search::evaluate(const Board& board, int alpha, int beta) {
    // Check for known drawn endgames
    if (Tablebase::EndgameRules::is_known_draw(board)) {
        return 0;
    }

    // Use lazy eval version
    int score = Eval::evaluate(board, alpha, beta);

    // Apply scale factor for drawish endgames
    int scaleFactor = Tablebase::EndgameRules::scale_factor(board);
    if (scaleFactor != 128) {
        score = score * scaleFactor / 128;
    }

    return score;
}

// UCI Output
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

    // MultiPV index (1-based for UCI protocol)
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

    // Output PV line
    // Note: PV should already be validated by caller before passing to report_info
    // We still do basic validation but don't stop on first invalid move if PV is from cache
    std::cout << " pv";
    Board tempBoard = board;
    int movesOutput = 0;
    for (int i = 0; i < pv.length && i < MAX_PLY; ++i) {
        Move m = pv.moves[i];
        if (m == MOVE_NONE) break;

        // Try to validate move, but if we already output some moves, be more lenient
        if (!MoveGen::is_pseudo_legal(tempBoard, m) || !MoveGen::is_legal(tempBoard, m)) {
            break;  // Stop at first illegal move
        }

        std::cout << " " << move_to_string(m);
        movesOutput++;

        // Make the move on temp board to validate next move in sequence
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
