#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

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
                LMRTable[d][m] = static_cast<int>(0.5 + std::log(d) * std::log(m) / 2.0);
            }
        }
    }
}

// ============================================================================
// Pruning & Extension Constants (TAHAP 3)
// ============================================================================

// Futility pruning margins per depth
constexpr int FutilityMargin[7] = { 0, 100, 200, 300, 400, 500, 600 };

// Razoring margins per depth
constexpr int RazorMargin[4] = { 0, 200, 400, 600 };

// Reverse futility pruning (static null move) margins
constexpr int RFPMargin[7] = { 0, 80, 160, 240, 320, 400, 480 };

// Late move pruning thresholds (skip quiet moves after this many tries at low depth)
constexpr int LMPThreshold[8] = { 0, 5, 8, 12, 17, 23, 30, 38 };

// Extension limits
constexpr int MAX_EXTENSIONS = 16;

// Multi-Cut parameters
constexpr int MULTI_CUT_DEPTH = 5;
constexpr int MULTI_CUT_COUNT = 3;   // Number of moves to try
constexpr int MULTI_CUT_REQUIRED = 2; // Number of cutoffs required

// Singular extension parameters
constexpr int SINGULAR_DEPTH = 6;
constexpr int SINGULAR_MARGIN = 64;   // Score margin for singularity

// ============================================================================
// Search Constructor
// ============================================================================

Search::Search() : stopped(false), searching(false), rootBestMove(MOVE_NONE),
                   rootPonderMove(MOVE_NONE), rootDepth(0), rootPly(0), optimumTime(0),
                   maximumTime(0), previousMove(MOVE_NONE) {
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
        stack[i].nullMovePruned = false;
    }
}

void Search::clear_history() {
    killers.clear();
    counterMoves.clear();
    history.clear();
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

    // Try opening book first (if not in analysis mode)
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

    // Try tablebase at root (if available and position is endgame)
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
    if (limits.movetime > 0) {
        optimumTime = limits.movetime;
        maximumTime = limits.movetime;
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

    // Basic time allocation
    optimumTime = time_left / moves_to_go + inc * 3 / 4;
    maximumTime = std::min(time_left / 4, optimumTime * 5);

    // Don't use more than available
    optimumTime = std::min(optimumTime, time_left - 50);
    maximumTime = std::min(maximumTime, time_left - 50);

    // Minimum time
    optimumTime = std::max(optimumTime, 10);
    maximumTime = std::max(maximumTime, 10);
}

void Search::check_time() {
    // [PERBAIKAN] Selalu cek stopped flag terlebih dahulu agar engine responsif terhadap stop command
    if (stopped) return;

    // Skip time checking for infinite/ponder mode, but still respect stop flag
    if (limits.infinite || limits.ponder) return;

    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed >= maximumTime) {
        stopped = true;
    }

    if (limits.nodes > 0 && searchStats.nodes >= limits.nodes) {
        stopped = true;
    }
}

bool Search::should_stop() const {
    return stopped;
}

// ============================================================================
// Iterative Deepening
// ============================================================================

void Search::iterative_deepening(Board& board) {
    rootBestMove = MOVE_NONE;
    rootPly = board.game_ply();  // Store starting ply for relative ply calculation
    int alpha = -VALUE_INFINITE;
    int beta = VALUE_INFINITE;
    int score = 0;

    // Generate root moves
    MoveList rootMoves;
    MoveGen::generate_legal(board, rootMoves);

    if (rootMoves.empty()) {
        return;  // No legal moves
    }

    // [PERBAIKAN] Set fallback move ke move legal pertama
    rootBestMove = rootMoves[0].move;

    if (rootMoves.size() == 1 && !limits.infinite) {
        return;  // Only one legal move
    }

    int maxDepth = limits.depth > 0 ? limits.depth : MAX_PLY;

    // Iterative deepening loop
    for (rootDepth = 1; rootDepth <= maxDepth && !stopped; ++rootDepth) {
        // Aspiration windows
        int delta = 20;

        if (rootDepth >= 5) {
            alpha = std::max(score - delta, -VALUE_INFINITE);
            beta = std::min(score + delta, VALUE_INFINITE);
        }

        while (true) {
            score = search(board, alpha, beta, rootDepth, false);

            if (stopped) break;

            if (score <= alpha) {
                // Fail low - widen window
                beta = (alpha + beta) / 2;
                alpha = std::max(score - delta, -VALUE_INFINITE);
                delta += delta / 2;
            } else if (score >= beta) {
                // Fail high - widen window
                beta = std::min(score + delta, VALUE_INFINITE);
                delta += delta / 2;
            } else {
                // Search completed within window
                break;
            }
        }

        if (!stopped) {
            // [PERBAIKAN] Hanya update jika PV tidak kosong
            if (pvLines[0].length > 0 && pvLines[0].moves[0] != MOVE_NONE) {
                rootBestMove = pvLines[0].moves[0];
            }
            // Get ponder move (2nd move in PV)
            rootPonderMove = (pvLines[0].length > 1) ? pvLines[0].moves[1] : MOVE_NONE;
            report_info(rootDepth, score, pvLines[0]);

            // Check if we've found a mate
            if (score >= VALUE_MATE_IN_MAX_PLY || score <= VALUE_MATED_IN_MAX_PLY) {
                // Continue searching for shorter mate if time permits
            }

            // Time management - check if we should stop early
            // [PERBAIKAN] Jangan stop early untuk infinite search
            if (!limits.infinite && limits.movetime == 0) {
                auto now = std::chrono::steady_clock::now();
                int elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                );

                if (elapsed > optimumTime / 2) {
                    break;  // Used more than half of optimum time
                }
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

    // Update selective depth
    if (ply > searchStats.selDepth) {
        searchStats.selDepth = ply;
    }

    // Check for time/node limits - check more frequently for responsiveness
    if ((searchStats.nodes & 1023) == 0) {
        check_time();
    }

    if (stopped) return 0;

    // Quiescence search at depth 0
    if (depth <= 0) {
        return qsearch(board, alpha, beta);
    }

    ++searchStats.nodes;

    // Mate distance pruning
    alpha = std::max(alpha, -VALUE_MATE + ply);
    beta = std::min(beta, VALUE_MATE - ply - 1);
    if (alpha >= beta) {
        return alpha;
    }

    // Initialize PV line for this ply
    pvLines[ply].clear();

    // Transposition table probe
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);
    Move ttMove = ttHit ? tte->move() : MOVE_NONE;
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
    bool inCheck = board.in_check();

    if (inCheck) {
        staticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        staticEval = tte->eval();
    } else {
        staticEval = evaluate(board);
    }

    // Razoring (at low depths, if eval is far below alpha)
    // Use table-based margins for more precise pruning
    if (!pvNode && !inCheck && depth <= 3 && depth >= 1) {
        int razorMargin = RazorMargin[depth];
        if (staticEval + razorMargin <= alpha) {
            int razorScore = qsearch(board, alpha - razorMargin, alpha - razorMargin + 1);
            if (razorScore <= alpha - razorMargin) {
                return razorScore;
            }
        }
    }

    // Reverse futility pruning / Static null move pruning
    // Use table-based margins for better tuning
    if (!pvNode && !inCheck && depth <= 6 && depth >= 1) {
        int rfpMargin = RFPMargin[depth];
        if (staticEval - rfpMargin >= beta && staticEval < VALUE_MATE_IN_MAX_PLY) {
            return staticEval;
        }
    }

    // Check if we have enough non-pawn material to do null move safely
    // This helps avoid zugzwang issues in pawn endgames
    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    // Get previous stack entry for double null move check
    SearchStack* ss = &stack[ply + 2];  // Adjusted for stack offset
    bool doubleNullMove = (ss->ply >= 1 && stack[ply + 1].nullMovePruned);

    // Null move pruning
    // Conditions: not PV, not in check, eval >= beta, have non-pawn material,
    // not after a null move (avoid double null move)
    if (!pvNode && !inCheck && staticEval >= beta && depth >= 3 &&
        hasNonPawnMaterial && !doubleNullMove) {

        // Dynamic reduction based on depth and eval
        int R = 3 + depth / 4 + std::min(3, (staticEval - beta) / 200);

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

    // Internal Iterative Deepening (IID)
    // If we have no hash move and high depth, do a shallow search first
    if (!ttMove && depth >= 6 && (pvNode || cutNode)) {
        int iidDepth = depth - 2;
        search(board, alpha, beta, iidDepth, cutNode);

        // Probe TT again
        tte = TT.probe(board.key(), ttHit);
        ttMove = ttHit ? tte->move() : MOVE_NONE;
    }

    // Move loop
    Move bestMove = MOVE_NONE;
    int bestScore = -VALUE_INFINITE;
    int moveCount = 0;
    Move quietsSearched[64];
    int quietCount = 0;

    // Track singularity for best move check
    bool singularSearched = false;

    MovePicker mp(board, ttMove, ply, killers, counterMoves, history, previousMove);
    Move m;

    while ((m = mp.next_move()) != MOVE_NONE) {
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

        // ====================================================================
        // Pre-move pruning (before making the move)
        // ====================================================================

        // Late Move Pruning (LMP)
        // Skip late quiet moves at low depths when we're not improving
        if (!pvNode && !inCheck && depth <= 7 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY) {
            if (moveCount > LMPThreshold[depth]) {
                continue;
            }
        }

        // Futility Pruning
        // Skip quiet moves that can't raise alpha even with optimistic margin
        if (!pvNode && !inCheck && depth <= 6 && depth >= 1 && !isCapture && !isPromotion &&
            bestScore > VALUE_MATED_IN_MAX_PLY && !givesCheck) {
            int futilityMargin = FutilityMargin[depth];
            if (staticEval + futilityMargin <= alpha) {
                // Skip this move - it won't raise alpha
                continue;
            }
        }

        // SEE pruning for captures
        // Skip losing captures at low depths
        if (!pvNode && depth <= 4 && isCapture && !SEE::see_ge(board, m, -50 * depth)) {
            continue;
        }

        // SEE pruning for quiet moves (prune if quiet move has very negative SEE)
        if (!pvNode && !inCheck && depth <= 3 && !isCapture && !SEE::see_ge(board, m, -100 * depth)) {
            continue;
        }

        // ====================================================================
        // Extensions
        // ====================================================================

        int extension = 0;
        int currentExtensions = (ss->ply >= 2 && ply >= 2) ? stack[ply + 1].extensions : 0;

        // Check extension (limited to MAX_EXTENSIONS total in path)
        if (givesCheck && currentExtensions < MAX_EXTENSIONS) {
            extension = 1;
        }

        // Passed pawn extension (for pawn moves to 7th rank)
        if (movedPt == PAWN && currentExtensions < MAX_EXTENSIONS) {
            Rank toRank = relative_rank(us, m.to());
            if (toRank == RANK_7) {
                extension = std::max(extension, 1);
            }
        }

        // Singular extension
        // If the TT move is singular (much better than alternatives), extend it
        if (!singularSearched && depth >= SINGULAR_DEPTH && isTTMove &&
            ttHit && ttBound != BOUND_UPPER && ttDepth >= depth - 3 &&
            std::abs(ttScore) < VALUE_MATE_IN_MAX_PLY && currentExtensions < MAX_EXTENSIONS) {

            singularSearched = true;
            int singularBeta = std::max(ttScore - SINGULAR_MARGIN * depth / 8, -VALUE_MATE);
            int singularDepth = (depth - 1) / 2;

            // Search with excluded move to see if TT move is singular
            ss->excludedMove = m;
            int singularScore = search(board, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = MOVE_NONE;

            if (singularScore < singularBeta) {
                // TT move is singular - extend it
                extension = 1;
            } else if (singularBeta >= beta) {
                // Multi-cut: if the singular search already found a beta cutoff,
                // we can return beta early
                return singularBeta;
            }
        }

        int newDepth = depth - 1 + extension;

        // Track extensions in path
        if (ss->ply >= 0 && ss->ply < MAX_PLY) {
            stack[ply + 2].extensions = currentExtensions + extension;
        }

        // ====================================================================
        // Late Move Reductions (LMR)
        // ====================================================================

        int reduction = 0;
        if (depth >= 3 && moveCount > 1 && !isCapture && !isPromotion) {
            reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];

            // Reduce less in PV nodes
            if (pvNode) {
                reduction -= 1;
            }

            // Reduce more in cut nodes
            if (cutNode) {
                reduction += 1;
            }

            // Reduce less if in check
            if (inCheck) {
                reduction -= 1;
            }

            // Reduce less if gives check
            if (givesCheck) {
                reduction -= 1;
            }

            // Reduce less for killer/counter moves
            if (killers.is_killer(ply, m) ||
                (previousMove && m == counterMoves.get(board.piece_on(previousMove.to()), previousMove.to()))) {
                reduction -= 1;
            }

            // Adjust by history
            int histScore = history.get(board.side_to_move(), m);
            reduction -= histScore / 5000;

            // Don't reduce below 1 or into qsearch
            reduction = std::clamp(reduction, 0, newDepth - 1);
        }

        // Make the move
        StateInfo si;
        board.do_move(m, si);

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
                // Update PV
                pvLines[ply].update(m, pvLines[ply + 1]);

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

    return bestScore;
}

// ============================================================================
// Quiescence Search
// ============================================================================

int Search::qsearch(Board& board, int alpha, int beta) {
    ++searchStats.nodes;

    // Check for time limits
    if ((searchStats.nodes & 2047) == 0) {
        check_time();
    }

    if (stopped) return 0;

    // Calculate relative ply from root position
    int ply = board.game_ply() - rootPly;
    bool inCheck = board.in_check();

    // Stand pat
    int staticEval = inCheck ? -VALUE_INFINITE : evaluate(board);

    if (!inCheck) {
        if (staticEval >= beta) {
            return staticEval;
        }
        if (staticEval > alpha) {
            alpha = staticEval;
        }
    }

    // Generate captures (and evasions if in check)
    MoveList moves;
    if (inCheck) {
        MoveGen::generate_evasions(board, moves);
    } else {
        MoveGen::generate_captures(board, moves);
    }

    // TT probe for move ordering
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);
    Move ttMove = ttHit ? tte->move() : MOVE_NONE;

    MovePicker mp(board, ttMove, history);
    Move m;
    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;
    int moveCount = 0;

    while ((m = mp.next_move()) != MOVE_NONE) {
        if (!MoveGen::is_legal(board, m)) {
            continue;
        }

        ++moveCount;

        // Delta pruning
        if (!inCheck && !m.is_promotion()) {
            int captureValue = PieceValue[type_of(board.piece_on(m.to()))];
            if (staticEval + captureValue + 200 < alpha) {
                continue;  // Can't raise alpha
            }
        }

        // SEE pruning
        if (!inCheck && !SEE::see_ge(board, m, 0)) {
            continue;  // Losing capture
        }

        // Make move
        StateInfo si;
        board.do_move(m, si);

        int score = -qsearch(board, -beta, -alpha);

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

    // Checkmate detection
    if (inCheck && moveCount == 0) {
        return -VALUE_MATE + ply;
    }

    return bestScore;
}

// ============================================================================
// Static Evaluation - Using Advanced HCE from eval.hpp
// ============================================================================

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

// ============================================================================
// UCI Output
// ============================================================================

void Search::report_info(int depth, int score, const PVLine& pv) {
    auto now = std::chrono::steady_clock::now();
    U64 elapsed = static_cast<U64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed == 0) elapsed = 1;

    U64 nps = searchStats.nodes * 1000 / elapsed;

    std::cout << "info";
    std::cout << " depth " << depth;
    std::cout << " seldepth " << searchStats.selDepth;

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
    std::cout << " pv " << pv.to_string();
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
        info.pv = pv;
        infoCallback(info);
    }
}
