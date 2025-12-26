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
    for (int d = 0; d < 64; ++d) {
        for (int m = 0; m < 64; ++m) {
            if (d == 0 || m == 0) {
                LMRTable[d][m] = 0;
            } else {
                // Tunable LMR formula: base + log(d) * log(m) / divisor
                // More aggressive reductions for late moves at high depths
                double reduction = SearchParams::LMR_BASE + std::log(d) * std::log(m) / SearchParams::LMR_DIVISOR;
                LMRTable[d][m] = static_cast<int>(reduction);
            }
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

Search::Search() : stopped(false), searching(false), rootBestMove(MOVE_NONE),
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

    if (limits.infinite || limits.ponder) return;

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

    if (rootMoves.size() == 1 && !limits.infinite) {
        return;  // Only one legal move
    }

    int maxDepth = limits.depth > 0 ? limits.depth : MAX_PLY;

    // Get MultiPV count from UCI options, clamped to legal move count
    int multiPV = std::min(UCI::options.multiPV, static_cast<int>(rootMoves.size()));
    multiPV = std::max(1, std::min(multiPV, MAX_MULTI_PV));

    // Initialize stability calculation members
    previousRootScore = VALUE_NONE;
    previousRootBestMove = MOVE_NONE;

    // Iterative deepening loop
    for (rootDepth = 1; rootDepth <= maxDepth && !stopped; ++rootDepth) {

        // Save previous scores and subtree nodes for all root moves ordering
        for (auto& rm : rootMoves) {
            rm.previousScore = rm.score;
            rm.prevSubtreeNodes = rm.subtreeNodes;
            rm.subtreeNodes = 0; // Reset for current iteration
        }

        // MultiPV loop - search each PV line
        for (pvIdx = 0; pvIdx < multiPV && !stopped; ++pvIdx) {

            // Set aspiration window based on previous score
            int alpha = -VALUE_INFINITE;
            int beta = VALUE_INFINITE;
            int delta = 50;
            int score = rootMoves[pvIdx].previousScore;

            // Use aspiration windows at higher depths
            if (rootDepth >= 5 && score != -VALUE_INFINITE) {
                alpha = std::max(score - delta, -VALUE_INFINITE);
                beta = std::min(score + delta, VALUE_INFINITE);
            }

            if (pvIdx > 0) {
                int prevScore = rootMoves[pvIdx - 1].score;
                beta = std::min(beta, prevScore + 1);
                alpha = std::min(alpha, prevScore - delta);
            }

            int failedHighLow = 0;

            while (true) {
                score = search(board, alpha, beta, rootDepth, false);

                if (stopped) break;

                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

                if (score <= alpha) {
                    // Fail low - widen alpha
                    beta = (alpha + beta) / 2;
                    alpha = std::max(score - delta, -VALUE_INFINITE);
                    delta *= 2;
                    ++failedHighLow;

                    if (failedHighLow >= 3) {
                        alpha = -VALUE_INFINITE;
                        beta = VALUE_INFINITE;
                    }
                } else if (score >= beta) {
                    // Fail high - widen beta
                    beta = std::min(score + delta, VALUE_INFINITE);
                    delta *= 2;
                    ++failedHighLow;

                    if (failedHighLow >= 3) {
                        alpha = -VALUE_INFINITE;
                        beta = VALUE_INFINITE;
                    }
                } else {
                    // Search completed within window
                    break;
                }
            }

            // Sort moves after completing this PV (keep previous PVs stable)
            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

            if (!stopped) {
                // Report this PV line
                const RootMove& rm = rootMoves[pvIdx];
                report_info(board, rootDepth, rm.score, rm.pv, pvIdx + 1);  // pvIdx is 0-based, UCI multipv is 1-based
            }
        }

        // After searching all PVs, update root best move from first PV
        if (!stopped && !rootMoves.empty()) {
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

                    // Stability-based Early Termination
                    // If best move has been stable for many iterations, stop early
                    auto now = std::chrono::steady_clock::now();
                    int elapsed = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                    );

                    // Calculate stability bonus (reduces required time)
                    double stabilityFactor = 1.0 - (bestMoveStability * 0.08);  // Up to 80% reduction
                    stabilityFactor = std::max(stabilityFactor, 0.3);  // Never less than 30%

                    int effectiveOptimum = static_cast<int>(optimumTime * stabilityFactor);

                    // Early stop conditions
                    if (multiPV == 1) {
                        // [PERBAIKAN] Jangan stop early saat mate score
                        // Saat menemukan mate, tetap lanjutkan analisis untuk:
                        // 1. Memastikan mate distance akurat
                        // 2. Mencari mate lebih pendek jika ada
                        bool isMateScore = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;

                        if (!isMateScore) {
                            // Very stable best move with enough depth - can stop early
                            if (bestMoveStability >= 5 && elapsed > effectiveOptimum * 0.4) {
                                break;
                            }

                            // Normal stability check
                            if (!failingLow && elapsed > effectiveOptimum * 0.6) {
                                break;
                            }

                            // Failing low but past soft limit
                            if (failingLow && elapsed > optimumTime * 0.9) {
                                break;
                            }
                        } else {
                            // Untuk mate score, hanya stop jika sudah mate in 1-2
                            // dan stable, untuk memastikan tidak ada miss
                            int mateIn = std::abs(VALUE_MATE - std::abs(score));
                            if (mateIn <= 4 && bestMoveStability >= 3 && elapsed > optimumTime * 0.8) {
                                break;  // Mate in 1-2 moves is definitive
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
    if ((searchStats.nodes & 1023) == 0) {
        check_time();
    }

    if (stopped) return 0;

    pvLines[ply].clear();

    // Get stack pointer for current ply
    // ply is guaranteed < MAX_PLY here
    SearchStack* ss = &stack[ply + 2];

    // Quiescence search at depth 0
    // [PERBAIKAN] Start qsearch with depth 4 for deeper tactical analysis
    if (depth <= 0) {
        return qsearch(board, alpha, beta, 4);
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

    // TT cutoff (only in non-PV nodes)
    if (!pvNode && ttHit && ttDepth >= depth) {
        if ((ttBound == BOUND_EXACT) ||
            (ttBound == BOUND_LOWER && ttScore >= beta) ||
            (ttBound == BOUND_UPPER && ttScore <= alpha)) {
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
    // Use corrected eval for more accurate pruning decisions
    if (!pvNode && !inCheck && depth <= 3 && depth >= 1) {
        int razorMargin = RazorMargin[depth];

        // Relax margin if improving
        if (improving) razorMargin += 50;

        if (correctedStaticEval + razorMargin <= alpha) {
            int razorScore = qsearch(board, alpha - razorMargin, alpha - razorMargin + 1);
            if (razorScore <= alpha - razorMargin) {
                return razorScore;
            }
        }
    }

    // Reverse futility pruning / Static null move pruning
    // Use corrected eval for more accurate pruning
    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMargin = RFPMargin[depth];

        // Stricter margin if not improving (prune less)
        // Correction: Improving -> Prune less (Increase margin)
        // Not Improving -> Prune more (Standard margin)
        if (improving) rfpMargin += 50;

        if (correctedStaticEval - rfpMargin >= beta && correctedStaticEval < VALUE_MATE_IN_MAX_PLY) {
            return correctedStaticEval;
        }
    }

    // Check if we have enough non-pawn material to do null move safely
    // This helps avoid zugzwang issues in pawn endgames
    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    bool doubleNullMove = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].nullMovePruned);

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
            if (depth >= 12) {
                int verifyScore = search(board, beta - 1, beta, depth - R - 1, false);
                if (verifyScore >= beta) {
                    return nullScore;
                }
            } else {
                return nullScore;
            }
        }
    }

    // Multi-Cut Pruning
    if (!pvNode && !inCheck && depth >= MULTI_CUT_DEPTH && cutNode) {
        int multiCutCount = 0;
        int movesTried = 0;

        // Generate and score moves for multi-cut check
        MoveList mcMoves;
        MoveGen::generate_all(board, mcMoves);

        // Try the first few moves with a shallow search
        for (size_t i = 0; i < mcMoves.size() && movesTried < MULTI_CUT_COUNT + 2; ++i) {
            Move m = mcMoves[i].move;

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
    if (!ttMove && depth >= IIR_MIN_DEPTH) {
        if (pvNode) {
            depth -= IIR_PV_REDUCTION;
        } else if (cutNode) {
            depth -= IIR_CUT_REDUCTION;
        } else {
            depth -= IIR_REDUCTION;
        }
    }

    // Move loop
    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

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

        // Enhanced Threat Detection
        bool createsThreat = false;
        bool createsFork = false;
        bool escapesAttack = false;  // [NEW] True if this move saves a piece from attack

        // [NEW] Check if the piece we're moving is under attack (escaping danger)
        // This is CRITICAL to avoid losing valuable pieces like the queen
        {
            Bitboard attackersToFrom = board.attackers_to(m.from(), board.pieces()) & board.pieces(~us);
            if (attackersToFrom) {
                // Our piece is being attacked - check if it's valuable enough to prioritize escape
                int pieceValueMoving = PieceValue[movedPt];

                // Find the least valuable attacker
                int minAttackerValue = 20000;  // Start high (king value)
                for (Bitboard atk = attackersToFrom; atk; ) {
                    Square atkSq = pop_lsb(atk);
                    PieceType atkPt = type_of(board.piece_on(atkSq));
                    minAttackerValue = std::min(minAttackerValue, PieceValue[atkPt]);
                }

                // If our piece is worth more than the attacker, this is a dangerous position
                // Mark as escaping attack if we're moving a valuable piece that's under threat
                if (pieceValueMoving >= minAttackerValue || movedPt == QUEEN || movedPt == ROOK) {
                    // Check if destination is safe (not attacked or defended)
                    Bitboard newOcc = board.pieces() ^ square_bb(m.from()) | square_bb(m.to());
                    Bitboard attackersToTo = board.attackers_to(m.to(), newOcc) & board.pieces(~us);

                    // If destination is safer (fewer/weaker attackers), this is an escape move
                    if (!attackersToTo || popcount(attackersToTo) < popcount(attackersToFrom)) {
                        escapesAttack = true;
                    }
                    // Also consider if the piece is defended at the destination
                    Bitboard defendersAtTo = board.attackers_to(m.to(), newOcc) & board.pieces(us);
                    if (defendersAtTo && pieceValueMoving <= 500) {  // Minor pieces and pawns
                        escapesAttack = true;
                    }
                }

                // [CRITICAL] Queen under attack should ALWAYS prioritize escape
                if (movedPt == QUEEN) {
                    escapesAttack = true;
                }
            }
        }

        if (!isCapture && !givesCheck) {
            // Calculate attacks from the destination square
            Bitboard newOccupied = board.pieces() ^ square_bb(m.from());
            Bitboard attacksAfter = attacks_bb(movedPt, m.to(), newOccupied);

            // Valuable enemy pieces (king excluded - that would be check)
            Bitboard valuableEnemies = board.pieces(~us, QUEEN) | board.pieces(~us, ROOK);
            Bitboard allMinorsAndUp = valuableEnemies | board.pieces(~us, BISHOP) | board.pieces(~us, KNIGHT);

            // Check for threats to valuable pieces
            Bitboard threatened = attacksAfter & valuableEnemies;
            if (threatened) {
                createsThreat = true;
            }

            // Check for forks (attacking 2+ minor/major pieces)
            Bitboard forkedPieces = attacksAfter & allMinorsAndUp;
            if (popcount(forkedPieces) >= 2) {
                createsFork = true;
                createsThreat = true;  // Fork implies threat
            }

            // Also consider discovered attack potential
            Bitboard ourSliders = board.pieces(us, BISHOP, QUEEN) | board.pieces(us, ROOK, QUEEN);
            Bitboard enemyKing = square_bb(board.king_square(~us));
            for (Bitboard sliders = ourSliders; sliders; ) {
                Square sliderSq = pop_lsb(sliders);
                if (sliderSq == m.from()) continue;  // We're moving this piece

                Bitboard sliderAttacks = attacks_bb(type_of(board.piece_on(sliderSq)), sliderSq, newOccupied);
                if (sliderAttacks & enemyKing) {
                    if (sliderAttacks & board.pieces(~us, QUEEN)) {
                        createsThreat = true;
                    }
                }
            }
        }

        // Skip late quiet moves at low depths when we're not improving
        // [PERBAIKAN] Skip LMP for moves that give check, create threats, OR escape attacks
        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            !givesCheck && !createsThreat && !escapesAttack && bestScore > VALUE_MATED_IN_MAX_PLY) {
            if (moveCount > LMPThreshold[depth]) {
                continue;
            }
        }

        // [PERBAIKAN] Don't prune moves that escape attacks on valuable pieces
        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck && !createsThreat && !escapesAttack) {
            int futilityMargin = FutilityMargin[depth];
            if (correctedStaticEval + futilityMargin <= alpha) {
                // Skip this move - it won't raise alpha
                continue;
            }
        }

        // SEE pruning for captures
        // Skip losing captures at low depths
        // [PERBAIKAN] Don't prune captures that win material (even just a pawn)
        if (!pvNode && depth <= 4 && isCapture) {
            Piece capturedPiece = board.piece_on(m.to());
            PieceType capturedType = (capturedPiece != NO_PIECE) ? type_of(capturedPiece) :
                                     (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);

            // If capturing anything of value (pawn or more), be very lenient with SEE pruning
            // This prevents missing free material
            int seeThreshold;
            if (capturedType >= PAWN && capturedType <= QUEEN) {
                // For real captures, use much more lenient threshold (-100 * depth)
                // This means we keep captures unless they lose a LOT of material
                seeThreshold = improving ?
                    -100 * depth :
                    -80 * depth;
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
        // Skip SEE pruning for checking/threatening moves OR moves that escape attacks
        if (!pvNode && !inCheck && depth <= 3 && !isCapture && !givesCheck && !createsThreat && !escapesAttack) {
            int seeThreshold = improving ?
                -SEE_QUIET_IMPROVING_FACTOR * depth :
                -SEE_QUIET_NOT_IMPROVING_FACTOR * depth;
            if (!SEE::see_ge(board, m, seeThreshold)) {
                continue;
            }
        }

        // History Leaf Pruning
        // [PERBAIKAN] Don't prune escape moves based on history alone
        if (!pvNode && !inCheck && depth <= HISTORY_LEAF_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck && !createsThreat && !escapesAttack &&
            bestScore > VALUE_MATED_IN_MAX_PLY) {
            int histScore = history.get(board.side_to_move(), m);
            if (histScore < -HISTORY_LEAF_PRUNING_MARGIN * depth) {
                continue;  // Skip moves with very bad history
            }
        }

        // =====================================================================
        // COUNTERMOVE HISTORY PRUNING
        // Prune quiet moves with poor countermove history scores.
        // This uses the 1-ply continuation history to identify moves that are
        // historically bad responses to the opponent's previous move.
        // =====================================================================
        if (!pvNode && !inCheck && depth <= COUNTER_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck && !createsThreat && !escapesAttack &&
            bestScore > VALUE_MATED_IN_MAX_PLY && contHist1ply) {

            PieceType pt = type_of(movedPiece);
            int cmHistScore = contHist1ply->get(pt, m.to());

            if (cmHistScore < -COUNTER_HIST_PRUNING_MARGIN * depth) {
                continue;  // Skip moves with very poor countermove history
            }
        }

        // =====================================================================
        // FOLLOW-UP HISTORY PRUNING
        // Prune quiet moves based on 4-ply continuation history pattern.
        // This captures longer-term move sequence patterns, pruning moves
        // that have historically been poor follow-ups to our move 2-ply ago.
        // =====================================================================
        if (!pvNode && !inCheck && depth <= FOLLOWUP_HIST_PRUNING_DEPTH &&
            !isCapture && !isPromotion && !givesCheck && !createsThreat && !escapesAttack &&
            bestScore > VALUE_MATED_IN_MAX_PLY && ply >= 4 && stack[ply - 2].contHistory) {

            const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
            PieceType pt = type_of(movedPiece);
            int followupScore = contHist4ply->get(pt, m.to());

            if (followupScore < -FOLLOWUP_HIST_PRUNING_MARGIN * depth) {
                continue;  // Skip moves with very poor follow-up history
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

        // Passed pawn extension (for pawn moves to 7th rank)
        if (movedPt == PAWN && currentExtensions < MAX_EXTENSIONS) {
            Rank toRank = relative_rank(us, m.to());
            if (toRank == RANK_7) {
                extension = std::max(extension, 1);
            }
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

        int newDepth = depth - 1 + extension;

        if (ss->ply >= 0 && ss->ply < MAX_PLY) {
            stack[ply + 2].extensions = currentExtensions + extension;
            stack[ply + 2].doubleExtensions = doubleExtensions;
        }

        int reduction = 0;
        // [PERBAIKAN] Langkah yang memberikan skak atau membuat ancaman TIDAK BOLEH
        // direduksi sama sekali, bukan hanya dikurangi nilai reduksinya.
        // Ini mencegah blunder taktis fatal dari reduksi langkah tajam.
        if (depth >= 2 && moveCount > 1 && !isCapture && !isPromotion && !givesCheck && !createsThreat) {
            reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];


            if (cutNode) {
                reduction += 1;  // Reduced from +2 to preserve tactics
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

            // [DIHAPUS] givesCheck dan createsThreat sudah dikecualikan di kondisi awal
            // Tidak perlu mengurangi reduksi di sini lagi

            if (createsFork) {
                reduction -= 1;
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
        }

        // Make the move
        Piece movedPiece2 = board.piece_on(m.from());
        if (ply + 2 < MAX_PLY + 4) {
            stack[ply + 2].contHistory = contHistory.get_entry(movedPiece2, m.to());
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
                    // Beta cutoff

                    // Update killer moves (only for quiet moves)
                    if (!isCapture) {
                        killers.store(ply, m);

                        // Update counter move
                        if (previousMove) {
                            counterMoves.store(board.piece_on(m.from()), m.to(), m);
                        }

                        // Update history
                        history.update_quiet_stats(board.side_to_move(), m,
                                                   quietsSearched, quietCount - 1, depth);

                        // Update continuation history
                        int bonus = depth * depth;
                        PieceType pt = type_of(movedPiece);

                        // Update 1-ply ago continuation history
                        if (contHist1ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), bonus);
                        }

                        // Update 2-ply ago continuation history
                        if (contHist2ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(pt, m.to(), bonus);
                        }

                        // Penalty for other quiet moves that were tried
                        for (int i = 0; i < quietCount - 1; ++i) {
                            if (quietsSearched[i] != m) {
                                Piece qpc = board.piece_on(quietsSearched[i].from());
                                PieceType qpt = type_of(qpc);
                                Square qto = quietsSearched[i].to();

                                if (contHist1ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -bonus);
                                }
                                if (contHist2ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -bonus);
                                }
                            }
                        }
                    } else {
                        // Update capture history using CaptureHistory class
                        Piece captured = board.piece_on(m.to());
                        if (captured != NO_PIECE) {
                             PieceType capturedPt = type_of(captured);
                             // Use update_capture_stats with gravity decay
                             captureHist.update_capture_stats(movedPiece, m.to(), capturedPt, depth, true);
                        }
                    }

                    break;  // Fail high
                }

                alpha = score;
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
    Bound bound = bestScore >= beta ? BOUND_LOWER :
                  bestScore > alpha ? BOUND_EXACT : BOUND_UPPER;

    if (tte) {
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation());
    }

    // Update Correction History
    // When we have a reliable search result (exact bound or fail high/low),
    // update the correction history to improve future static eval accuracy
    if (!inCheck && staticEval != VALUE_NONE && depth >= 3) {
        // Calculate the difference between search result and static eval
        // Positive diff means eval was too pessimistic, negative means too optimistic
        int diff = bestScore - staticEval;

        // Only update on reliable results (not when we failed low with no moves tried)
        if (bound == BOUND_EXACT ||
            (bound == BOUND_LOWER && bestScore >= beta) ||
            (bound == BOUND_UPPER && moveCount > 0)) {
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
    if ((searchStats.nodes & 2047) == 0) {
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

        // [PERBAIKAN] Generate quiet checks at first qsearch plies
        if (qsDepth > 0) {
            MoveList quiets;
            MoveGen::generate_quiets(board, quiets);

            // Filter for only moves that give check
            for (size_t i = 0; i < quiets.size(); ++i) {
                Move m = quiets[i].move;
                if (MoveGen::gives_check(board, m)) {
                    quietChecks.add(m, 0);
                }
            }
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

    // Use MovePicker with capture history for better ordering in qsearch
    MovePicker mp(board, ttMoves, ttMoveCount, history, &captureHist);
    Move m;
    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;

    // First search captures (or evasions if in check)
    while ((m = mp.next_move()) != MOVE_NONE) {
        if (!MoveGen::is_legal(board, m)) {
            continue;
        }

        // Hitung semua legal moves jika dalam skak (untuk detect skakmat)
        if (inCheck) {
            ++legalMoveCount;
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

        if (!inCheck && !m.is_promotion()) {
            // Never prune captures of valuable pieces (Queen, Rook, or any piece worth >= pawn)
            // This ensures we don't miss free material
            if (capturedPt != QUEEN && capturedPt != ROOK && capturedPt != KNIGHT && capturedPt != BISHOP) {
                // Use very large margin (500) to be conservative
                // Only prune if we're way below alpha and the capture can't possibly help
                if (staticEval + captureValue + 500 < alpha) {
                    continue;  // Can't raise alpha
                }
            }
        }

        // SEE pruning (hanya saat TIDAK dalam skak)
        // [PERBAIKAN] JANGAN PERNAH prune capture Queen - pengorbanan ratu sering kunci taktik
        bool seeWinning = (captureValue >= PieceValue[PAWN]);
        int seeThreshold = seeWinning ? -200 : -100;

        // Skip SEE pruning untuk:
        // 1. Saat dalam skak (semua evasion harus dicek)
        // 2. Capture yang menguntungkan (seeWinning)
        // 3. Capture Queen (pengorbanan ratu sering kunci taktik)
        if (!inCheck && !seeWinning && capturedPt != QUEEN && !SEE::see_ge(board, m, seeThreshold)) {
            continue;  // Losing capture - but only if not capturing real material or queen
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

    // [PERBAIKAN] Validate PV line before output
    // Output only valid moves to prevent "Illegal PV move" warnings from cutechess
    std::cout << " pv";
    Board tempBoard = board;
    for (int i = 0; i < pv.length; ++i) {
        Move m = pv.moves[i];
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
