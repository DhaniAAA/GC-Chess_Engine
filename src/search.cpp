#include "search.hpp"
#include "thread.hpp"
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

inline bool is_aligned(Square s1, Square s2, Square s3) {
    return (LineBB[s1][s2] & square_bb(s3)) != 0;
}

Search Searcher;

int LMRTable[64][64];

void init_lmr_table() {
    for (int depth = 0; depth < 64; ++depth) {
        for (int move = 0; move < 64; ++move) {
            if (depth < 3 || move == 0) {
                LMRTable[depth][move] = 0;
                continue;
            }

            double reduction = SearchParams::LMR_BASE + (std::log(depth) * std::log(move)) / SearchParams::LMR_DIVISOR;
            LMRTable[depth][move] = static_cast<int>(reduction);
        }
    }
}

using namespace SearchParams;

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

void Search::compute_selective_budget(int depth) {
    (void)depth;
    for (auto& rm : rootMoves) {
        rm.selectiveSkip  = false;
        rm.selectiveDepth = 0;
    }
    return;
}


Search::Search() : stopped(false), isPondering(false), rootBestMove(MOVE_NONE),
                   rootPonderMove(MOVE_NONE), previousRootBestMove(MOVE_NONE), rootBestScore(VALUE_NONE),
                   previousRootScore(VALUE_NONE),
                   rootDepth(0), rootPly(0), pvIdx(0),
                   optimumTime(0), maximumTime(0) {
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
        stack[i].tripleExtensions = 0;
        stack[i].fractionalExt = 0;
        stack[i].nullMovePruned = false;
        stack[i].inLMR = false;
        stack[i].reduction = 0;
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

void Search::start(Board& board, const SearchLimits& lim) {
    limits = lim;
    stopped = false;
    completedDepth = 0;
    searchStats.reset();

    killers.clear();
    mateKillers.clear();
    for (int i = 0; i < MAX_PLY + 4; ++i) {
        stack[i].currentMove = MOVE_NONE;
        stack[i].excludedMove = MOVE_NONE;
        stack[i].staticEval = VALUE_NONE;
        stack[i].correctedStaticEval = VALUE_NONE;
        stack[i].extensions = 0;
        stack[i].doubleExtensions = 0;
        stack[i].tripleExtensions = 0;
        stack[i].fractionalExt = 0;
        stack[i].nullMovePruned = false;
        stack[i].inLMR = false;
        stack[i].reduction = 0;
        stack[i].contHistory = nullptr;
    }

    if (threadId == 0 && !standalone) {
        TT.new_search();
    }

    init_time_management(board.side_to_move());
    startTime = std::chrono::steady_clock::now();

    if (threadId == 0 && !standalone && !limits.infinite && Book::book.is_loaded()) {
        Move bookMove = Book::book.probe(board);
        if (bookMove != MOVE_NONE) {
            rootBestMove = bookMove;
            completedDepth = 1;
            if (!silentMode) {
                std::cout << "info string Book move: " << move_to_string(bookMove) << std::endl;
                std::cout << "info depth 1 score cp 0 nodes 0 time 0 pv "
                          << move_to_string(bookMove) << std::endl;
            }
            return;
        }
    }

    if (threadId == 0 && Tablebase::TB.is_initialized() && Tablebase::TB.can_probe(board)) {
        Move tbMove = Tablebase::TB.probe_root(board);
        if (tbMove != MOVE_NONE) {
            rootBestMove = tbMove;
            completedDepth = 100;
            Tablebase::WDLScore wdl = Tablebase::TB.probe_wdl(board);
            if (!silentMode) {
                std::cout << "info string Tablebase hit: " << move_to_string(tbMove) << std::endl;
                int score = Tablebase::Tablebases::wdl_to_score(wdl, 0);
                std::cout << "info depth 100 score cp " << score << " nodes 0 time 0 pv "
                          << move_to_string(tbMove) << std::endl;
            }
            return;
        }
    }

    iterative_deepening(board);

    isPondering = false;
}

void Search::on_ponderhit() {
    isPondering = false;
    limits.ponder = false;
    startTime = std::chrono::steady_clock::now();
}

void Search::init_time_management(Color us) {
    int moveOverhead = std::clamp(UCI::options.moveOverhead, 0, 1000);

    bestMoveStability = 0;
    failLowCount = 0;
    lastFailLowScore = VALUE_NONE;
    emergencyMode = false;
    positionComplexity = 50;

    if (limits.movetime > 0) {
        optimumTime = std::max(1, limits.movetime - moveOverhead);
        maximumTime = std::max(1, limits.movetime - moveOverhead);
        baseOptimumTime = optimumTime;
        return;
    }

    if (limits.time[us] == 0) {
        optimumTime = 1000000;
        maximumTime = 1000000;
        baseOptimumTime = optimumTime;
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
        baseOptimumTime = optimumTime;
        return;
    } else if (time_left < 2000) {
        emergencyMode = true;
        optimumTime = std::max(20, time_left / 15 + inc / 2);
        maximumTime = std::max(50, time_left / 8 + inc / 2);
        baseOptimumTime = optimumTime;
        return;
    } else if (time_left < 5000) {
        emergencyMode = false;
        optimumTime = time_left / 12 + inc * 2 / 3;
        maximumTime = time_left / 6 + inc;

        optimumTime = std::min(optimumTime, safeTime - 100);
        maximumTime = std::min(maximumTime, safeTime - 50);

        optimumTime = std::max(optimumTime, 30);
        maximumTime = std::max(maximumTime, 60);
        baseOptimumTime = optimumTime;
        return;
    }

    optimumTime = safeTime / moves_to_go + inc * 3 / 4;
    maximumTime = std::min(safeTime / 3, optimumTime * 5);

    optimumTime = std::min(optimumTime, safeTime - 100);
    maximumTime = std::min(maximumTime, safeTime - 50);

    optimumTime = std::max(optimumTime, 50);
    maximumTime = std::max(maximumTime, 100);
    baseOptimumTime = optimumTime;
}

void Search::check_time() {
    if (stopped) return;
    if (threadId != 0 && !standalone) return;

    if (limits.infinite || limits.ponder || isPondering) return;

    // "go depth N" tanpa clock: hanya batasi oleh node limit (maximumTime
    // sudah di-set 1.000.000 ms oleh init_time_management sebagai safety net).
    bool depthOnly = limits.depth > 0 && limits.time[WHITE] == 0 &&
                     limits.time[BLACK] == 0 && limits.movetime == 0;

    U64 totalNodes = (standalone || Threads.thread_count() <= 1) ? searchStats.nodes : Threads.total_nodes();
    if (limits.nodes > 0 && totalNodes >= limits.nodes) {
        stopped = true;
        if (!standalone) Threads.stop();
        return;
    }

    if (depthOnly) return;

    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed >= maximumTime) {
        stopped = true;
        if (!standalone) Threads.stop();
    }
}

bool Search::should_stop() const {
    return stopped;
}

void Search::iterative_deepening(Board& board) {
    rootBestMove = MOVE_NONE;
    rootPonderMove = MOVE_NONE;
    rootBestScore = VALUE_NONE;
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

    {
        Color us = board.side_to_move();
        Color them = ~us;
        Square enemyKingSq = board.king_square(them);
        Bitboard enemyKingZone = king_attacks_bb(enemyKingSq) | square_bb(enemyKingSq);
        Bitboard occupied = board.pieces();
        Bitboard enemyQueens = board.pieces(them, QUEEN);
        Bitboard enemyRooksQueens = board.pieces(them, ROOK, QUEEN);
        Bitboard enemyNonPawns = board.pieces(them) ^ board.pieces(them, PAWN, KING);
        Bitboard our7thPawns = board.pieces(us, PAWN) & ((us == WHITE) ? RANK_7_BB : RANK_2_BB);
        Bitboard promoBlockSquares = (us == WHITE) ? (our7thPawns << 8) : (our7thPawns >> 8);

        for (auto& rm : rootMoves) {
            Move m = rm.move;
            Square from = m.from();
            Square to = m.to();
            Piece pc = board.piece_on(from);
            PieceType pt = type_of(pc);
            bool isCapture = board.piece_on(to) != NO_PIECE || m.is_enpassant();
            bool givesCheck = MoveGen::gives_check(board, m);
            bool nearKing = (enemyKingZone & square_bb(to)) != 0;

            bool isDiscoveredAttack = false;
            Bitboard enemyHeavy = board.pieces(~us, ROOK, QUEEN);
            if (enemyHeavy) {
                Bitboard rAtk = rook_attacks_bb(from, occupied);
                Bitboard friendlyR = rAtk & board.pieces(us, ROOK, QUEEN);
                Bitboard enemyR = rAtk & enemyHeavy;
                if (friendlyR && enemyR) {
                    Square fSq = lsb(friendlyR);
                    Square eSq = lsb(enemyR);
                    if (is_aligned(fSq, from, eSq) && !is_aligned(from, to, eSq)) {
                        isDiscoveredAttack = true;
                    }
                }
                if (!isDiscoveredAttack) {
                    Bitboard bAtk = bishop_attacks_bb(from, occupied);
                    Bitboard friendlyB = bAtk & board.pieces(us, BISHOP, QUEEN);
                    Bitboard enemyB = bAtk & enemyHeavy;
                    if (friendlyB && enemyB) {
                        Square fSq = lsb(friendlyB);
                        Square eSq = lsb(enemyB);
                        if (is_aligned(fSq, from, eSq) && !is_aligned(from, to, eSq)) {
                            isDiscoveredAttack = true;
                        }
                    }
                }
            }

            int initScore = 1000;
            if (isCapture && givesCheck && nearKing) {
                initScore = 10000;
            } else if (isCapture && givesCheck) {
                initScore = 9000;
            } else if (isCapture && isDiscoveredAttack) {
                initScore = 8500;
            } else if (isCapture && nearKing) {
                initScore = 7500;
            } else if (isCapture && SEE::see_ge(board, m, 0)) {
                initScore = 6000 + std::clamp(mvv_lva(board, m) / 4, -500, 1500);
            } else if (isCapture && (pt == KNIGHT || pt == BISHOP) &&
                       (relative_rank(us, to) >= RANK_6 || SEE::see_ge(board, m, -PieceValue[KNIGHT]))) {
                initScore = 5800 + std::clamp(mvv_lva(board, m) / 8, -500, 1000);
            } else if (givesCheck) {
                initScore = 5000;
            } else if (isCapture) {
                initScore = 3500 + std::clamp(mvv_lva(board, m) / 8, -500, 1000);
            } else {
                Bitboard occAfter = occupied ^ square_bb(from);
                Bitboard atk = (pt == PAWN) ? pawn_attacks_bb(us, to) : attacks_bb(pt, to, occAfter);
                if (promoBlockSquares & square_bb(from)) {
                    initScore += 3200;
                }
                if (atk & enemyKingZone) {
                    initScore += 2200;
                }
                if (pt == PAWN && (atk & enemyQueens)) {
                    initScore += 3500;
                }
                if ((pt == PAWN && (atk & enemyNonPawns)) ||
                    ((pt == KNIGHT || pt == BISHOP) && (atk & (enemyNonPawns | board.pieces(them, PAWN)))) ||
                    (pt == ROOK && (atk & enemyQueens)) ||
                    (pt == QUEEN && (atk & enemyRooksQueens))) {
                    initScore += 2400;
                }
                if (isDiscoveredAttack) {
                    initScore += 2600;
                }
                if (pt == PAWN && relative_rank(us, to) >= RANK_6) {
                    initScore += 2000;
                }
                Bitboard ourAdvPawns = board.pieces(us, PAWN) &
                                       ((us == WHITE) ? (RANK_6_BB | RANK_7_BB) : (RANK_3_BB | RANK_2_BB));
                if (ourAdvPawns) {
                    Bitboard pPawns = ourAdvPawns;
                    while (pPawns) {
                        Square psq = pop_lsb(pPawns);
                        if (!(Eval::passed_pawn_mask(us, psq) & board.pieces(~us, PAWN))) {
                            if (king_attacks_bb(psq) & square_bb(to)) {
                                initScore += 2400;
                                break;
                            }
                        }
                    }
                }
                initScore += std::clamp(history.get(us, m) / 16, -400, 800);
            }
            rm.score = -VALUE_INFINITE;
            rm.previousScore = -VALUE_INFINITE + initScore;
        }
        std::stable_sort(rootMoves.begin(), rootMoves.end());
    }

    rootBestMove = rootMoves[0].move;

    if (rootMoves.size() == 1 && !limits.infinite) {
        pvIdx = 0;
        rootDepth = 1;

        int score = search(board, -VALUE_INFINITE, VALUE_INFINITE, 1, false);

        if (!stopped) {
            rootMoves[0].score = score;
            rootBestScore = score;

            if (rootMoves[0].pv.length == 0) {
                rootMoves[0].pv.moves[0] = rootMoves[0].move;
                rootMoves[0].pv.length = 1;
            }
            report_info(rootBoard, 1, score, rootMoves[0].pv, 1);

            if (!silentMode) {
                std::cout << "info string Only one legal move" << std::endl;
            }
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

    int startDepth = (threadId == 0 || standalone) ? 1 : (1 + (threadId % 3));
    for (rootDepth = startDepth; rootDepth <= maxDepth && !stopped; ++rootDepth) {
        if (!standalone && threadId > 0 && rootDepth > 4 && ((rootDepth + threadId) % 4 == 0)) {
            continue;
        }
        for (auto& rm : rootMoves) {
            if (rm.score != -VALUE_INFINITE) {
                rm.previousScore = rm.score;
            }
            rm.prevSubtreeNodes = rm.subtreeNodes;
            rm.subtreeNodes = 0;
        }

        std::vector<RootMove> rootMovesBackup = rootMoves;

        for (auto& rm : rootMoves) {
            rm.score = -VALUE_INFINITE;
            rm.pv.clear();
        }

        // Selective search: allocate depth budget per root move based on
        // score/node ratio from the previous iteration. Always active.
        compute_selective_budget(rootDepth);

        for (pvIdx = 0; pvIdx < multiPV && !stopped; ++pvIdx) {
            Move analyzedMove = rootMoves[pvIdx].move;
            PVLine previousPVLine = rootMoves[pvIdx].pv;
            int previousIterScore = rootMoves[pvIdx].score;

            int alpha = -VALUE_INFINITE;
            int beta = VALUE_INFINITE;
            int delta = ASPIRATION_INITIAL_DELTA;
            int score = rootMoves[pvIdx].previousScore;
            int failCount = 0;
            int searchDepth = rootDepth;

            // Selective search: skip or reduce depth for clearly worse root moves.
            // pvIdx==0 (current best) is always searched fully.
            if (pvIdx > 0) {
                const RootMove& selRM = rootMoves[pvIdx];
                if (selRM.selectiveSkip) {
                    if (selRM.selectiveDepth == -1) {
                        // Tier 4 — Hard skip: move is hopelessly behind, skip entirely.
                        // Score stays as previousScore so ordering is preserved.
                        rootMoves[pvIdx].score = selRM.previousScore;
                        continue;
                    }
                    // Tier 3 — Soft skip: depth=1 to keep TT/history warm.
                    score = search(board, -VALUE_INFINITE, VALUE_INFINITE, 1, false);
                    if (!stopped) {
                        rootMoves[pvIdx].score = score;
                    }
                    continue;
                }
                if (selRM.selectiveDepth > 0) {
                    searchDepth = std::max(1, rootDepth - selRM.selectiveDepth);
                }
            }

            bool prevIsMate = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;

            if (rootDepth >= ASPIRATION_MIN_DEPTH && score > -VALUE_INFINITE + 20000 && !prevIsMate) {
                alpha = std::max(score - delta, -VALUE_INFINITE);
                beta = std::min(score + delta, VALUE_INFINITE);
            }

            if (pvIdx > 0) {
                int prevScore = rootMoves[pvIdx - 1].score;
                beta = std::min(beta, prevScore + 1);
                alpha = std::min(alpha, prevScore - delta);
            }

            while (true) {
                if (failCount > 0 && !limits.infinite && limits.movetime == 0) {
                    auto now = std::chrono::steady_clock::now();
                    int elapsed = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                    );
                    if (elapsed > maximumTime * 9 / 10) {
                        alpha = -VALUE_INFINITE;
                        beta = VALUE_INFINITE;
                        searchDepth = rootDepth;
                    }
                }

                score = search(board, alpha, beta, searchDepth, false);

                if (stopped) break;

                if (score > alpha) {
                    std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());
                }

                if (score <= alpha) {
                    failCount++;
                    delta += delta / 2;

                    if (delta > 500 || std::abs(score) >= VALUE_MATE_IN_MAX_PLY) {
                        alpha = -VALUE_INFINITE;
                        beta = VALUE_INFINITE;
                    } else {
                        alpha = std::max(score - delta, -VALUE_INFINITE);
                    }

                    searchDepth = rootDepth;

                } else if (score >= beta) {
                    failCount++;
                    delta += delta / 2;

                    if (delta > 500 || std::abs(score) >= VALUE_MATE_IN_MAX_PLY) {
                        beta = VALUE_INFINITE;
                        alpha = -VALUE_INFINITE;
                    } else {
                        beta = std::min(score + delta, VALUE_INFINITE);
                    }

                    searchDepth = rootDepth;

                } else {
                    break;
                }
            }

            if (!stopped && failCount > 0 && !silentMode) {
                std::cout << "info string depth " << rootDepth
                          << " aspiration fails " << failCount << std::endl;
            }

            if (stopped) {
                rootMoves = rootMovesBackup;
                break;
            }

            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());

            if (analyzedMove != MOVE_NONE) {
                for (auto& rm : rootMoves) {
                    if (rm.move == analyzedMove) {
                        // Only restore old PV when PV vanished but score is stable
                        // (search glitch). Empty PV + big drop is a real tactical
                        // refutation and must NOT be hidden, otherwise WAC bm is wrong.
                        bool scoreStable = (previousIterScore != -VALUE_INFINITE) &&
                                           (std::abs(rm.score - previousIterScore) <= 150);
                        bool pvDisappeared = (rm.pv.length == 0) && (previousPVLine.length > 0);

                        if (pvDisappeared && scoreStable) {
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

                    if (pvIdx == 0 || rm.score > overallBestScore || overallBestMove == MOVE_NONE) {
                        overallBestMove = rm.move;
                        overallBestPV = rm.pv;
                        overallBestScore = rm.score;
                    }

                    report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                }
                else {
                    uint16_t moveKey = rm.move.raw();
                    auto it = bestKnownPVPerMove.find(moveKey);

                    bool scoreStable = (it != bestKnownPVPerMove.end()) &&
                                       (rm.score != -VALUE_INFINITE) &&
                                       (std::abs(rm.score - it->second.second) <= 150);

                    if (scoreStable && it->second.first.length > 0) {
                        rm.pv = it->second.first;
                        rm.score = it->second.second;
                        report_info(rootBoard, rootDepth, rm.score, rm.pv, pvIdx + 1);
                    }
                    else {
                        if (it != bestKnownPVPerMove.end()) {
                            bestKnownPVPerMove.erase(it);
                        }
                        PVLine reportPV = rm.pv;
                        if (reportPV.length == 0 && rm.move != MOVE_NONE) {
                            reportPV.moves[0] = rm.move;
                            reportPV.length = 1;
                        }
                        report_info(rootBoard, rootDepth, rm.score, reportPV, pvIdx + 1);
                    }
                }
            }
        }

        if (!rootMoves.empty()) {
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
                            bool scoreStable = (rm.score != -VALUE_INFINITE) &&
                                               (std::abs(rm.score - it->second.second) <= 150);
                            if (scoreStable) {
                                rm.pv = it->second.first;
                                rm.score = it->second.second;
                            }
                        }
                    }
                }
            }

            const RootMove& bestRM = rootMoves[0];

            if (bestRM.pv.length > 0 && bestRM.pv.moves[0] != MOVE_NONE) {
                Move candidate = bestRM.pv.moves[0];
                if (MoveGen::is_pseudo_legal(board, candidate) && MoveGen::is_legal(board, candidate)) {
                    rootBestMove = candidate;
                    rootBestScore = bestRM.score;
                }
            } else if (bestRM.move != MOVE_NONE &&
                       MoveGen::is_pseudo_legal(board, bestRM.move) &&
                       MoveGen::is_legal(board, bestRM.move)) {
                rootBestMove = bestRM.move;
                rootBestScore = bestRM.score;
            }
            else if (overallBestMove != MOVE_NONE &&
                     MoveGen::is_pseudo_legal(board, overallBestMove) &&
                     MoveGen::is_legal(board, overallBestMove)) {
                rootBestMove = overallBestMove;
                rootBestScore = overallBestScore;
            }

            if (previousRootBestMove != MOVE_NONE &&
                ((bestMoveStability >= 2 &&
                  type_of(board.piece_on(rootBestMove.from())) == KING &&
                  type_of(board.piece_on(previousRootBestMove.from())) != KING) ||
                 (bestMoveStability >= 3))) {
                for (const auto& rm : rootMoves) {
                    if (rm.move == previousRootBestMove) {
                        int prevScore = (rm.score != -VALUE_INFINITE) ? rm.score : rm.previousScore;
                        int allowedDelta = 30;
                        if (prevScore != -VALUE_INFINITE && rootBestScore - prevScore <= allowedDelta) {
                            rootBestMove = rm.move;
                            rootBestScore = prevScore;
                            if (rm.pv.length > 0) pvLines[0] = rm.pv;
                            break;
                        }
                    }
                }
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
            completedDepth = rootDepth;

            if (threadId == 0) {
                if (previousRootBestMove != MOVE_NONE) {
                    if (rootBestMove == previousRootBestMove) {
                        bestMoveStability = std::min(bestMoveStability + 1, 10);
                    } else {
                        bestMoveStability = 0;
                    }
                }
                previousRootScore = rootBestScore;
                previousRootBestMove = rootBestMove;
            }

            if (threadId == 0 && !limits.infinite && !stopped && limits.movetime == 0 && !emergencyMode && limits.depth == 0) {
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
                    int failLowExtension = 0;
                    if (previousRootScore != VALUE_NONE) {
                        int scoreDrop = previousRootScore - score;

                        if (scoreDrop > 30) {
                            failLowCount++;
                            failingLow = true;
                            lastFailLowScore = score;
                            failLowExtension += std::min(scoreDrop * 2, baseOptimumTime / 2);
                        } else if (scoreDrop < -20) {
                            failLowCount = 0;
                        }

                        if (failLowCount >= 2) {
                            failLowExtension += baseOptimumTime / 3;
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

                        double complexityFactor = 0.75 + (positionComplexity / 200.0);
                        int adjustedOptimum = static_cast<int>(baseOptimumTime * complexityFactor) + failLowExtension;
                        optimumTime = std::min(maximumTime, adjustedOptimum);
                    }

                    auto now = std::chrono::steady_clock::now();
                    int elapsed = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
                    );

                    scoreHistory[scoreHistoryIdx] = score;
                    scoreHistoryIdx = (scoreHistoryIdx + 1) % 6;

                    if (previousRootScore != VALUE_NONE) {
                        int scoreDelta = std::abs(score - previousRootScore);
                        if (scoreDelta > 50) {
                            lastBigScoreChange = rootDepth;
                        }
                    }
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

                    double stabilityFactor = 1.0 - (bestMoveStability * 0.05);
                    stabilityFactor = std::max(stabilityFactor, 0.5);
                    int effectiveOptimum = static_cast<int>(optimumTime * stabilityFactor);

                    const int MIN_DEPTH_FOR_EARLY_STOP = 10;
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
                            int matePlies = VALUE_MATE - std::abs(score);
                            int mateIn = (matePlies + 1) / 2;

                            if (score > 0 && rootDepth >= std::max(4, matePlies + 2) && bestMoveStability >= 2) {
                                break;
                            }
                            if (rootDepth >= MIN_DEPTH_FOR_EARLY_STOP &&
                                mateIn <= 6 && bestMoveStability >= 3 &&
                                elapsed > optimumTime * 0.4) {
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

int Search::search(Board& board, int alpha, int beta, int depth, bool cutNode) {
    PROFILE_FUNCTION();
    const bool pvNode = (beta - alpha) > 1;
    const int origAlpha = alpha;

    int ply = board.game_ply() - rootPly;

    if (UNLIKELY(ply >= MAX_PLY)) {
        return evaluate(board);
    }
    if (ply > searchStats.selDepth) {
        searchStats.selDepth = ply;
    }

    TT.prefetch(board.key());

    const U64 nodeMask = (limits.nodes > 0 && limits.nodes < 16384) ? 255 : 16383;
    if (UNLIKELY((searchStats.nodes & nodeMask) == 0)) {
        check_time();
    }

    if (UNLIKELY(stopped)) return 0;

    pvLines[ply].clear();

    SearchStack* ss = &stack[ply + 2];

    ss->cutoffCnt = 0;

    Move previousMove = ply >= 0 ? stack[ply + 1].currentMove : MOVE_NONE;

    if (depth <= 0) {
        return qsearch(board, alpha, beta, 0);
    }

    ++searchStats.nodes;

    if (UNLIKELY(ply > 0 && board.is_draw(ply))) {
        return 0;
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
    if (ttHit) {
        TT.get_moves(board.key(), ttMoves, ttMoveCount);
    }

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
    bool allowTTCutoff = !pvNode && ttHit && ttDepth >= depth && ss->excludedMove == MOVE_NONE;

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
                pvLines[ply].length = 1;
                pvLines[ply].moves[0] = ttMove;
            }
            return ttScore;
        }
    }

    int staticEval;
    int correctedStaticEval;
    bool inCheck = board.in_check();
    ss->inCheck = inCheck;
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

    if (!inCheck && ss->staticEval != VALUE_NONE) {
        if (ply >= 2 && stack[ply].staticEval != VALUE_NONE) {
            improving = (ss->staticEval > stack[ply].staticEval);
        } else if (ply >= 4 && stack[ply - 2].staticEval != VALUE_NONE) {
            improving = (ss->staticEval > stack[ply - 2].staticEval);
        }
    }

    Color enemy = ~us;
    bool usIsBareKing = (board.pieces(us) == square_bb(board.king_square(us)));
    bool themIsBareKing = (board.pieces(enemy) == square_bb(board.king_square(enemy)));
    bool bareKingEndgame = usIsBareKing || themIsBareKing;

    int postLMRReduction = 0;

    Bitboard ourAdvancedPawns = board.pieces(us, PAWN) &
                                ((us == WHITE) ? (RANK_6_BB | RANK_7_BB) : (RANK_3_BB | RANK_2_BB));
    Bitboard enemy7thPawns = (board.pieces(QUEEN) == 0)
                           ? (board.pieces(enemy, PAWN) & ((enemy == WHITE) ? RANK_7_BB : RANK_2_BB))
                           : EMPTY_BB;
    bool opponentJustEvadedCheck = (ply >= 1 && stack[ply + 1].inCheck);

    // Safe Razoring at depth 1 only (margin 450 so minor-piece sacrifices are never razored)
    if (!pvNode && !inCheck && !usIsBareKing && !ourAdvancedPawns && !opponentJustEvadedCheck &&
        depth == 1 &&
        ss->excludedMove == MOVE_NONE && std::abs(alpha) < VALUE_MATE_IN_MAX_PLY) {
        if (correctedStaticEval + 450 <= alpha) {
            int qVal = qsearch(board, alpha, alpha + 1, 0);
            if (qVal <= alpha) {
                return qVal;
            }
        }
    }

    if (!pvNode && !inCheck && !usIsBareKing && !enemy7thPawns && depth <= RFP_MAX_DEPTH && depth >= 1
        && ss->excludedMove == MOVE_NONE) {
        int rfpMarg = rfp_margin(depth, improving);

        if (correctedStaticEval - rfpMarg >= beta
            && correctedStaticEval < VALUE_MATE_IN_MAX_PLY
            && std::abs(beta) < VALUE_MATE_IN_MAX_PLY) {
            bool wasInCheckPrevTurn = (ply >= 2 && stack[ply].inCheck);
            bool kingRingUnderHeavyAttack = wasInCheckPrevTurn || opponentJustEvadedCheck;
            if (!kingRingUnderHeavyAttack && previousMove != MOVE_NONE) {
                Square prevTo = previousMove.to();
                Piece prevPc = board.piece_on(prevTo);
                if (prevPc != NO_PIECE && color_of(prevPc) == enemy) {
                    PieceType prevPt = type_of(prevPc);
                    Bitboard occ = board.pieces();
                    Bitboard prevAtk = (prevPt == PAWN)
                        ? pawn_attacks_bb(enemy, prevTo)
                        : attacks_bb(prevPt, prevTo, occ);
                    Bitboard ourValuable = board.pieces(us) ^ board.pieces(us, PAWN);
                    if (prevAtk & ourValuable) {
                        kingRingUnderHeavyAttack = true;
                    }
                }
            }
            if (!kingRingUnderHeavyAttack && depth >= 2) {
                Square ourKSq = board.king_square(us);
                Bitboard ourKingRing = king_attacks_bb(ourKSq) | square_bb(ourKSq);
                Bitboard occ = board.pieces();
                Bitboard enemyHeavy = board.pieces(enemy, QUEEN, ROOK);
                while (enemyHeavy) {
                    Square sq = pop_lsb(enemyHeavy);
                    PieceType pt = type_of(board.piece_on(sq));
                    if (attacks_bb(pt, sq, occ) & ourKingRing) {
                        kingRingUnderHeavyAttack = true;
                        break;
                    }
                }
            }
            if (!kingRingUnderHeavyAttack) {
                return correctedStaticEval;
            }
        }
    }

    bool hasNonPawnMaterial = board.pieces(board.side_to_move()) !=
                              board.pieces(board.side_to_move(), PAWN, KING);

    bool doubleNullMove = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].nullMovePruned);
    bool mateThreat = false;

    if (!pvNode && !inCheck && correctedStaticEval >= beta && depth >= NULL_MOVE_MIN_DEPTH &&
        hasNonPawnMaterial && !doubleNullMove && ss->excludedMove == MOVE_NONE) {

        int R = NULL_MOVE_BASE_R + depth / NULL_MOVE_DEPTH_DIVISOR;

        if (correctedStaticEval - beta > NULL_MOVE_EVAL_MARGIN) {
            R += 1;
        }

        if (!improving) R++;

        R = std::min(R, depth - 1);

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
                int verifyDepth = depth - R;
                if (verifyDepth < 1) verifyDepth = 1;

                int verifyScore = search(board, beta - 1, beta, verifyDepth, false);
                if (verifyScore >= beta) {
                    return nullScore;
                }
            } else {
                return nullScore;
            }
        }
        else if (nullScore <= VALUE_MATED_IN_MAX_PLY) {
            mateThreat = true;
        }
    }

    const ContinuationHistoryEntry* contHist1ply = (ply >= 1 && ply + 1 < MAX_PLY + 4 && stack[ply + 1].contHistory) ?
                                                    stack[ply + 1].contHistory : nullptr;
    const ContinuationHistoryEntry* contHist2ply = (ply >= 2 && ply < MAX_PLY + 4 && stack[ply].contHistory) ?
                                                    stack[ply].contHistory : nullptr;
    const ContinuationHistoryEntry* contHist3ply = (ply >= 3 && ply - 1 >= 0 && stack[ply - 1].contHistory) ?
                                                    stack[ply - 1].contHistory : nullptr;
    const ContinuationHistoryEntry* contHist4ply = (ply >= 4 && ply - 2 >= 0 && stack[ply - 2].contHistory) ?
                                                    stack[ply - 2].contHistory : nullptr;

    if (!pvNode && !inCheck && !bareKingEndgame && !enemy7thPawns && depth >= 6 &&
        ss->excludedMove == MOVE_NONE &&
        correctedStaticEval != VALUE_NONE && correctedStaticEval >= beta - 100 &&
        std::abs(beta) < VALUE_MATE_IN_MAX_PLY) {

        int probCutBeta = std::max(beta, correctedStaticEval) + 200;
        int probCutDepth = std::max(1, depth - 4);

        MoveList captures;
        MoveGen::generate_captures(board, captures);

        for (size_t i = 0; i < captures.size(); ++i) {
            Move m = captures[i].move;

            if (previousMove != MOVE_NONE && m.to() == previousMove.to()) {
                continue;
            }

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

    int searchDepth = depth - postLMRReduction;
    if (searchDepth < 1 && depth >= 1) searchDepth = 1;
    if (!inCheck && !ttMove && depth >= IIR_MIN_DEPTH) {
        if (pvNode) {
            searchDepth -= IIR_PV_REDUCTION;
        } else if (cutNode) {
            searchDepth -= IIR_CUT_REDUCTION;
        } else if (ply > 2) {
            searchDepth -= IIR_REDUCTION;
        }
        if (searchDepth < 1) searchDepth = 1;
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

    MovePicker mp(board, ttMoves, ttMoveCount, ply, killers, counterMoves, history, previousMove,
                  contHist1ply, contHist2ply, &captureHist, contHist3ply, contHist4ply);

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

        if (rootNode && !silentMode) {
            auto now = std::chrono::steady_clock::now();
            int elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
            );
            // if (elapsed > 3000) {
            //     std::cout << "info depth " << depth
            //               << " currmove " << move_to_string(m)
            //               << " currmovenumber " << moveCount
            //               << std::endl;
            // }
        }

        bool isCapture = !board.empty(m.to()) || m.is_enpassant();
        bool isPromotion = m.is_promotion();
        bool isTTMove = (m == ttMove);

        bool givesCheck;
        bool attacksKingZone;
        if (mp.hasMoveInfo) {
            givesCheck = mp.givesCheckInfo;
            attacksKingZone = mp.attacksKingZoneInfo;
        } else {
            givesCheck = MoveGen::gives_check(board, m);
            attacksKingZone = false;
        }

        Piece movedPiece = board.piece_on(m.from());
        PieceType movedPt = type_of(movedPiece);
        Color us = board.side_to_move();

        ss->currentMove = m;

        bool isDiscoveredAttack = false;
        if (!givesCheck) {
            Square enemyKSq = board.king_square(~us);
            if (!isCapture && !mp.hasMoveInfo) {
                Bitboard kZone = king_attacks_bb(enemyKSq) | square_bb(enemyKSq);
                Bitboard kZone2 = kZone | shift<NORTH>(kZone) | shift<SOUTH>(kZone) |
                                  shift<EAST>(kZone) | shift<WEST>(kZone);
                Bitboard occ = board.pieces() ^ square_bb(m.from());
                Bitboard atk = (movedPt == PAWN) ? pawn_attacks_bb(us, m.to()) : attacks_bb(movedPt, m.to(), occ);
                Bitboard enemyTargets = (movedPt == QUEEN || movedPt == ROOK) ? kZone2 : kZone;
                if (movedPt == PAWN) enemyTargets |= (board.pieces(~us) ^ board.pieces(~us, PAWN, KING));
                 else if (movedPt == KNIGHT || movedPt == BISHOP) enemyTargets |= board.pieces(~us, ROOK, QUEEN);
                else if (movedPt == ROOK) enemyTargets |= board.pieces(~us, QUEEN);
                else if (movedPt == QUEEN) enemyTargets |= board.pieces(~us, ROOK, QUEEN);
                if (atk & enemyTargets) {
                    attacksKingZone = true;
                }
            }
            if (!isCapture && !attacksKingZone && (movedPt == ROOK || movedPt == BISHOP || movedPt == QUEEN)) {
                Bitboard ray = line_bb(m.to(), enemyKSq);
                if (ray) {
                    bool sliderMatchesRay = (movedPt == ROOK) ? (file_of(m.to()) == file_of(enemyKSq) || rank_of(m.to()) == rank_of(enemyKSq)) :
                                            (movedPt == BISHOP) ? (file_of(m.to()) != file_of(enemyKSq) && rank_of(m.to()) != rank_of(enemyKSq)) : true;
                    if (sliderMatchesRay) {
                        Bitboard between = between_bb(m.to(), enemyKSq);
                        Bitboard occ = board.pieces() ^ square_bb(m.from());
                        Bitboard blockers = between & occ;
                        if (blockers && (blockers & board.pieces(us)) == blockers && popcount(blockers) == 1) {
                            attacksKingZone = true;
                        }
                    }
                }
            }
            if ((board.blockers_for_king(~us) & square_bb(m.from())) &&
                !is_aligned(m.from(), m.to(), enemyKSq)) {
                isDiscoveredAttack = true;
            } else {
                Bitboard enemyHeavy = board.pieces(~us, ROOK, QUEEN);
                if (enemyHeavy) {
                    Bitboard rAtk = rook_attacks_bb(m.from(), board.pieces());
                    Bitboard friendlyR = rAtk & board.pieces(us, ROOK, QUEEN);
                    Bitboard enemyR = rAtk & enemyHeavy;
                    if (friendlyR && enemyR) {
                        Square fSq = lsb(friendlyR);
                        Square eSq = lsb(enemyR);
                        if (is_aligned(fSq, m.from(), eSq) && !is_aligned(m.from(), m.to(), eSq)) {
                            isDiscoveredAttack = true;
                        }
                    }
                    if (!isDiscoveredAttack) {
                        Bitboard bAtk = bishop_attacks_bb(m.from(), board.pieces());
                        Bitboard friendlyB = bAtk & board.pieces(us, BISHOP, QUEEN);
                        Bitboard enemyB = bAtk & enemyHeavy;
                        if (friendlyB && enemyB) {
                            Square fSq = lsb(friendlyB);
                            Square eSq = lsb(enemyB);
                            if (is_aligned(fSq, m.from(), eSq) && !is_aligned(m.from(), m.to(), eSq)) {
                                isDiscoveredAttack = true;
                            }
                        }
                    }
                }
            }
        }
        Bitboard enemyQueens = board.pieces(~us, QUEEN);
        bool attacksQueen = false;
        if (enemyQueens) {
            if (movedPt == PAWN) {
                attacksQueen = (pawn_attacks_bb(us, m.to()) & enemyQueens) != 0;
            } else {
                attacksQueen = (attacks_bb(movedPt, m.to(), board.pieces()) & enemyQueens) != 0;
            }
        }
        Bitboard enemyRooks = board.pieces(~us, ROOK);
        bool attacksRook = false;
        if (enemyRooks) {
            if (movedPt == PAWN) {
                attacksRook = (pawn_attacks_bb(us, m.to()) & enemyRooks) != 0;
            } else if (movedPt == KNIGHT || movedPt == BISHOP) {
                attacksRook = (attacks_bb(movedPt, m.to(), board.pieces()) & enemyRooks) != 0;
            }
        }
        bool isAdvancedPawnPush = (movedPt == PAWN && relative_rank(us, m.to()) >= RANK_5);
        bool supportsPassedPawn = false;
        if (ourAdvancedPawns) {
            Bitboard passPawns = ourAdvancedPawns;
            while (passPawns) {
                Square psq = pop_lsb(passPawns);
                if (!(Eval::passed_pawn_mask(us, psq) & board.pieces(~us, PAWN))) {
                    if (king_attacks_bb(psq) & square_bb(m.to())) {
                        supportsPassedPawn = true;
                        break;
                    }
                }
            }
        }
        bool isTacticalQuiet = attacksKingZone || isDiscoveredAttack || givesCheck || isAdvancedPawnPush || attacksQueen || attacksRook || supportsPassedPawn;

        bool previousWasViolent = false;
        if (previousMove != MOVE_NONE) {
            previousWasViolent = !board.empty(previousMove.to()) ||
                                  previousMove.is_enpassant() ||
                                  inCheck;
        }
        bool losingMaterial = (!inCheck && correctedStaticEval != VALUE_NONE &&
                               correctedStaticEval < -300);
        bool inViolentSequence = previousWasViolent && losingMaterial;
        // Only mate threat bypasses pruning; violent sequence no longer disables pruning
        bool tacticalBypass = mateThreat;

        if (!pvNode && !inCheck && bestScore > VALUE_MATED_IN_MAX_PLY) {

            if (isCapture || givesCheck) {
                if (isCapture && !givesCheck && depth <= SEE_CAPTURE_MAX_DEPTH) {
                    Square ekSq_cap = board.king_square(~us);
                    Bitboard ekZone_cap = king_attacks_bb(ekSq_cap) | square_bb(ekSq_cap);
                    bool captureNearKing = (ekZone_cap & square_bb(m.to())) != 0;
                    Piece capturedPiece = board.piece_on(m.to());
                    PieceType capturedType = (capturedPiece != NO_PIECE) ? type_of(capturedPiece) :
                                             (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);

                    if ((captureNearKing && tacticalBypass) || (capturedType >= KNIGHT && depth <= 4)) {
                    } else {

                        int seeThreshold;
                        if (capturedType >= PAWN && capturedType <= QUEEN) {
                            seeThreshold = improving ? -150 * depth : -130 * depth;
                        } else {
                            seeThreshold = improving ?
                                -SEE_CAPTURE_IMPROVING_FACTOR * depth :
                                -SEE_CAPTURE_NOT_IMPROVING_FACTOR * depth;
                        }

                        bool isEnemyAdvPawn = (capturedType == PAWN && relative_rank(~us, m.to()) >= RANK_6);
                        if (captureNearKing || (ourAdvancedPawns && relative_rank(us, m.to()) >= RANK_6) || isEnemyAdvPawn) {
                            seeThreshold *= 3;
                        }

                        if (!isTTMove && !SEE::see_ge(board, m, seeThreshold)) {
                            continue;
                        }
                    }
                }
            }
            else if (!isPromotion) {
                if (!tacticalBypass) {
                    if (depth <= LMP_MAX_DEPTH) {
                        int lmpThresh = lmp_threshold(depth, improving);
                        if (moveCount > lmpThresh) {
                            if (!isTacticalQuiet && !killers.is_killer(ply, m) && history.get(us, m) < 2000) {
                                continue;
                            }
                        }
                    }
                    if (!usIsBareKing && depth <= FUTILITY_MAX_DEPTH && depth >= 1) {
                        int futilMarg = futility_margin(depth, improving);
                        if (correctedStaticEval + futilMarg <= alpha) {
                            if (!isTacticalQuiet && !killers.is_killer(ply, m) && !isTTMove) {
                                continue;
                            }
                        }
                    }
                    if (!isTacticalQuiet && depth <= HISTORY_LEAF_PRUNING_DEPTH && moveCount > 4) {
                        int histScore = history.get(board.side_to_move(), m);
                        if (histScore < -HISTORY_LEAF_PRUNING_MARGIN * depth) {
                            continue;
                        }
                    }
                    if (!isTacticalQuiet && depth <= COUNTER_HIST_PRUNING_DEPTH && contHist1ply && moveCount > 5) {
                        PieceType pt = type_of(movedPiece);
                        int cmHistScore = contHist1ply->get(pt, m.to());

                        if (cmHistScore < -COUNTER_HIST_PRUNING_MARGIN * depth) {
                            continue;
                        }
                    }
                    if (!isTacticalQuiet && depth <= FOLLOWUP_HIST_PRUNING_DEPTH && ply >= 4 && stack[ply - 2].contHistory && moveCount > 6) {
                        const ContinuationHistoryEntry* contHist4ply = stack[ply - 2].contHistory;
                        PieceType pt = type_of(movedPiece);
                        int followupScore = contHist4ply->get(pt, m.to());

                        if (followupScore < -FOLLOWUP_HIST_PRUNING_MARGIN * depth) {
                            continue;
                        }
                    }

                    if (depth <= SEE_QUIET_MAX_DEPTH) {
                        int seeThreshold = improving ?
                            -SEE_QUIET_IMPROVING_FACTOR * depth :
                            -SEE_QUIET_NOT_IMPROVING_FACTOR * depth;
                        // King-zone/discovered-attack quiets are potential sacrifices
                        // (Qh6/Nxf7 class): never SEE-prune them, let search decide.
                        if (!isTacticalQuiet && !isTTMove && !killers.is_killer(ply, m) &&
                            !(previousMove && m == counterMoves.get(board.piece_on(previousMove.to()), previousMove.to())) &&
                            !SEE::see_ge(board, m, seeThreshold)) {
                            continue;
                        }
                    }
                }
            }
        }

        int currentExtensions = (ply >= 1) ? stack[ply + 1].extensions : 0;
        int doubleExtensions = (ply >= 1) ? stack[ply + 1].doubleExtensions : 0;
        int tripleExtensions = (ply >= 1) ? stack[ply + 1].tripleExtensions : 0;

        int fractionalExt = 0;

        if (givesCheck && currentExtensions < MAX_EXTENSIONS && depth >= 2) {
            bool seeZero = (mp.hasMoveInfo && (mp.lastFlags & MovePicker::FLAG_SEE_GE_ZERO)) ||
                           SEE::see_ge(board, m, 0);
            int minCheckExtDepth = (board.pieces(QUEEN) == 0) ? 4 : 6;
            if (seeZero ? (pvNode || depth >= minCheckExtDepth || (ply <= 4 && depth >= 2)) : (ply <= 2 && depth >= 4)) {
                fractionalExt += FRAC_EXT_CHECK;
            }
        }

        if (inCheck && currentExtensions < MAX_EXTENSIONS && moveCount == 1) {
            fractionalExt = std::max(fractionalExt, FRAC_EXT_SINGULAR);
        }

        if (movedPt == PAWN && currentExtensions < MAX_EXTENSIONS && depth >= 2) {
            Rank toRank = relative_rank(us, m.to());
            if (toRank == RANK_7) {
                fractionalExt = std::max(fractionalExt, FRAC_EXT_PASSED_PAWN);
            } else if (toRank == RANK_6 && !(Eval::passed_pawn_mask(us, m.to()) & board.pieces(~us, PAWN))) {
                fractionalExt = std::max(fractionalExt, FRAC_EXT_PASSED_PAWN);
            }
        }

        if (mateThreat && currentExtensions < MAX_EXTENSIONS && fractionalExt < FRAC_EXT_SCALE) {
            fractionalExt = std::max(fractionalExt, FRAC_EXT_MATE_THREAT);
        }

        if (!rootNode && ss->excludedMove == MOVE_NONE && !singularSearched && depth >= SINGULAR_DEPTH && isTTMove &&
            ttHit && ttBound != BOUND_UPPER && ttDepth >= depth - 3 &&
            std::abs(ttScore) < VALUE_MATE_IN_MAX_PLY && currentExtensions < MAX_EXTENSIONS) {

            singularSearched = true;

            int singularMargin = SINGULAR_MARGIN;

            int depthDiff = depth - ttDepth;
            if (depthDiff > 0) {
                singularMargin += depthDiff * SINGULAR_TT_DEPTH_PENALTY;
            }

            if (improving) {
                singularMargin -= SINGULAR_IMPROVING_BONUS;
            }
            if (pvNode) {
                singularMargin -= 5;
            }

            int singularBeta = std::max(ttScore - singularMargin * depth / 8, -VALUE_MATE);
            int singularDepth = std::min((depth - 3) / 2, 6);


            ss->excludedMove = m;
            int singularScore = search(board, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = MOVE_NONE;

            if (singularScore < singularBeta) {
                int singularMarginDiff = singularBeta - singularScore;

                fractionalExt = FRAC_EXT_SINGULAR;

                if (!pvNode) {
                    if (singularMarginDiff > SINGULAR_DOUBLE_EXT_MARGIN &&
                        doubleExtensions < DOUBLE_EXT_LIMIT) {
                        fractionalExt = FRAC_EXT_SINGULAR_DOUBLE;
                        ++doubleExtensions;
                        if (!isCapture && singularMarginDiff > SINGULAR_TRIPLE_EXT_MARGIN &&
                            tripleExtensions < TRIPLE_EXT_LIMIT) {
                            fractionalExt += FRAC_EXT_PV_MOVE;
                            ++tripleExtensions;
                        }
                    }
                }
            }

            else if (singularScore >= beta) {
                return singularScore;
            }
            else if (cutNode && depth >= NEG_EXT_MIN_DEPTH &&
                     singularScore < alpha - NEG_EXT_THRESHOLD) {
                fractionalExt = -FRAC_EXT_SINGULAR;
            }

            else if (ttScore >= beta) {
                fractionalExt = -FRAC_EXT_SINGULAR_DOUBLE;
            }
        }

        int extension = fractionalExt / FRAC_EXT_SCALE;
        extension = std::clamp(extension, -2, 2);

        if (extension > 0 && ply >= rootDepth * MAX_EXTENSION_PLY_RATIO) {
            extension = 0;
        }

        int newDepth = searchDepth - 1 + extension;

        if (newDepth < -2) {
            newDepth = -2;
        }

        if (ss->ply >= 0 && ss->ply < MAX_PLY) {
            stack[ply + 2].extensions = currentExtensions + std::max(0, extension);
            stack[ply + 2].doubleExtensions = doubleExtensions;
            stack[ply + 2].tripleExtensions = tripleExtensions;
            stack[ply + 2].fractionalExt = 0;
        }

        int reduction = 0;
        bool lmrApplied = false;

        if (depth >= LMR_MIN_DEPTH && moveCount > 1 && !isCapture && !isPromotion &&
            (!givesCheck || (!rootNode && !pvNode && moveCount > 4))) {
            bool isAdvancedPawnPush = false;
            if (movedPt == PAWN) {
                Rank toRank = relative_rank(us, m.to());
                if (toRank >= RANK_5) {
                    isAdvancedPawnPush = true;
                }
            }

            bool isKingApproachInMopUp = false;
            if (bareKingEndgame && movedPt == KING) {
                Square ekSq = board.king_square(enemy);
                int distBefore = std::abs(int(file_of(m.from())) - int(file_of(ekSq))) +
                                 std::abs(int(rank_of(m.from())) - int(rank_of(ekSq)));
                int distAfter = std::abs(int(file_of(m.to())) - int(file_of(ekSq))) +
                                std::abs(int(rank_of(m.to())) - int(rank_of(ekSq)));
                if (distAfter < distBefore) {
                    isKingApproachInMopUp = true;
                }
            }

            bool onMatePath = (bestScore >= VALUE_MATE_IN_MAX_PLY || bestScore <= VALUE_MATED_IN_MAX_PLY);

            if (!isAdvancedPawnPush && !isKingApproachInMopUp && !onMatePath && extension <= 0) {
                reduction = LMRTable[std::min(depth, 63)][std::min(moveCount, 63)];
                lmrApplied = true;

                if (cutNode) {
                    reduction += 1;
                    if (!ttMove) {
                        reduction += 1;
                    }
                }

                if (!improving) {
                    reduction += 1;
                }
                if (ply + 2 < MAX_PLY + 4) {
                    int childCutoffs = stack[ply + 2].cutoffCnt;
                    if (childCutoffs > 2) {
                        reduction += 1;
                    }
                }

                if (!isTacticalQuiet && moveCount > 16) {
                    reduction += 1;
                }

                if (pvNode) {
                    reduction -= 2;
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
                if (contHist3ply) {
                    statScore += contHist3ply->get(pt, m.to());
                }
                if (contHist4ply) {
                    statScore += contHist4ply->get(pt, m.to());
                }
                reduction -= std::clamp(statScore / HISTORY_LMR_DIVISOR,
                                        -HISTORY_LMR_MAX_ADJ, HISTORY_LMR_MAX_ADJ);

                if (mateThreat) {
                    reduction /= LMR_NMP_THREAT_DIVISOR;
                }

                if (inViolentSequence) {
                    reduction = std::min(reduction, 1);
                }

                if (givesCheck) {
                    reduction = rootNode ? 0 : std::min(reduction, 1);
                } else if (isTacticalQuiet) {
                     if (rootNode || (opponentJustEvadedCheck && ply <= 2) || (attacksKingZone && ply <= 2)) {
                        reduction = 0;
                    } else {
                        reduction = opponentJustEvadedCheck ? std::min(reduction, 1) : std::max(0, reduction - 2);
                    }
                }

                if (rootNode) {
                    reduction = 0;
                }

                if (inCheck) {
                    reduction = std::max(0, reduction - 1);
                }

                reduction = std::clamp(reduction, 0, newDepth - 1);
            }
        }
        else if (!rootNode && depth >= LMR_MIN_DEPTH && moveCount > LMR_CAPTURE_MIN_MOVE &&
                 isCapture && !isPromotion && !givesCheck && !isTTMove &&
                 extension <= 0 && bestScore > VALUE_MATED_IN_MAX_PLY) {

            Square ekSq_lmr = board.king_square(~us);
            Bitboard ekZone_lmr = king_attacks_bb(ekSq_lmr) | square_bb(ekSq_lmr);
            bool captureNearKing_lmr = (ekZone_lmr & square_bb(m.to())) != 0;
            bool attacksNearKing_lmr = (attacks_bb(movedPt, m.to(), board.pieces() ^ square_bb(m.from())) & ekZone_lmr) != 0;

            bool discTacticalAttack = false;
            Bitboard rAtk = rook_attacks_bb(m.from(), board.pieces());
            Bitboard friendlyR = rAtk & board.pieces(us, ROOK, QUEEN);
            Bitboard enemyTargetsR = ekZone_lmr | board.pieces(~us, QUEEN);
            if (friendlyR && (rAtk & enemyTargetsR)) {
                Square fSq = lsb(friendlyR);
                Square tSq = lsb(rAtk & enemyTargetsR);
                if (is_aligned(fSq, m.from(), tSq) && !is_aligned(m.from(), m.to(), tSq)) {
                    discTacticalAttack = true;
                }
            }
            if (!discTacticalAttack) {
                Bitboard bAtk = bishop_attacks_bb(m.from(), board.pieces());
                Bitboard friendlyB = bAtk & board.pieces(us, BISHOP, QUEEN);
                Bitboard enemyTargetsB = ekZone_lmr | board.pieces(~us, QUEEN);
                if (friendlyB && (bAtk & enemyTargetsB)) {
                    Square fSq = lsb(friendlyB);
                    Square tSq = lsb(bAtk & enemyTargetsB);
                    if (is_aligned(fSq, m.from(), tSq) && !is_aligned(m.from(), m.to(), tSq)) {
                        discTacticalAttack = true;
                    }
                }
            }

            reduction = LMR_CAPTURE_REDUCTION;
            lmrApplied = true;

            Piece capturedPiece_lmr = board.piece_on(m.to());
            PieceType capturedPt_lmr = (capturedPiece_lmr != NO_PIECE) ? type_of(capturedPiece_lmr) : (m.is_enpassant() ? PAWN : NO_PIECE_TYPE);
            bool isEnemyAdvPawn_lmr = (capturedPt_lmr == PAWN && relative_rank(~us, m.to()) >= RANK_6);

            if (captureNearKing_lmr || attacksNearKing_lmr || discTacticalAttack || isEnemyAdvPawn_lmr) {
                reduction = 0;
                lmrApplied = false;
            } else if (SEE::see_ge(board, m, PieceValue[PAWN])) {
                reduction = 0;
                lmrApplied = false;
            } else if (mp.hasMoveInfo && (mp.lastFlags & MovePicker::FLAG_SEE_GE_ZERO)) {
                int capHistScore = captureHist.get(movedPiece, m.to(), type_of(board.piece_on(m.to()))) / 1000;
                reduction -= std::clamp(capHistScore, -1, 1);
            } else if (!SEE::see_ge(board, m, 0)) {
                reduction += 1;
            } else {
                int capHistScore = captureHist.get(movedPiece, m.to(), type_of(board.piece_on(m.to()))) / 1000;
                reduction -= std::clamp(capHistScore, -1, 1);
            }

            if (pvNode) {
                reduction = 0;
                lmrApplied = false;
            }

            reduction = std::clamp(reduction, 0, newDepth - 2);
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
            ss->inLMR = (reduction > 0);
            ss->reduction = reduction;

            score = -search(board, -alpha - 1, -alpha, newDepth - reduction, rootNode ? false : true);

            if (score > alpha && reduction > 0) {
                if (!isCapture) {
                    int researchBonus = SearchParams::stat_bonus(depth) * LMR_RESEARCH_BONUS_SCALE;
                    researchBonus = std::min(researchBonus, 1200);
                    history.update_with_bonus(us, m, researchBonus);

                    PieceType pt = type_of(movedPiece);
                    if (contHist1ply) {
                        const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), researchBonus / 2);
                    }
                }

                ss->inLMR = false;
                ss->reduction = 0;
                score = -search(board, -alpha - 1, -alpha, newDepth, !cutNode);
            }

            if (score > alpha && (score < beta || rootNode)) {
                score = -search(board, -beta, -alpha, newDepth, false);
            }

            ss->inLMR = false;
            ss->reduction = 0;
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
            captureSEE[captureCount] = (mp.hasMoveInfo && (mp.lastFlags & MovePicker::FLAG_SEE_GE_ZERO)) ?
                                       1 : (SEE::see_ge(board, m, 0) ? 1 : -1);
            captureCount++;
        }

        bool isBetter = rootNode ? ((moveCount == 1) ? (score > bestScore) : (score > alpha && score > bestScore))
                                 : (score > bestScore);

        if (isBetter) {
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
                    int statBonus = SearchParams::stat_bonus(depth);
                    int statMalus = SearchParams::stat_malus(depth);

                    if (!isCapture) {
                        killers.store(ply, m);

                        if (score >= VALUE_MATE_IN_MAX_PLY) {
                            mateKillers.store(ply, m);
                        }

                        if (previousMove) {
                            counterMoves.store(board.piece_on(previousMove.to()), previousMove.to(), m);
                        }

                        history.update_quiet_stats(board.side_to_move(), m,
                                                   quietsSearched, quietCount - 1, depth);

                        PieceType pt = type_of(movedPiece);

                        if (contHist1ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(pt, m.to(), statBonus * CONT_HIST_1PLY_WEIGHT);
                        }
                        if (contHist2ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(pt, m.to(), statBonus * CONT_HIST_2PLY_WEIGHT);
                        }
                        if (contHist3ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist3ply)->update(pt, m.to(), statBonus * CONT_HIST_3PLY_WEIGHT);
                        }
                        if (contHist4ply) {
                            const_cast<ContinuationHistoryEntry*>(contHist4ply)->update(pt, m.to(), statBonus * CONT_HIST_4PLY_WEIGHT);
                        }


                        for (int i = 0; i < quietCount - 1; ++i) {
                            if (quietsSearched[i] != m) {
                                Piece qpc = board.piece_on(quietsSearched[i].from());
                                PieceType qpt = type_of(qpc);
                                Square qto = quietsSearched[i].to();

                                history.update(board.side_to_move(), quietsSearched[i], depth, false);

                                if (contHist1ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist1ply)->update(qpt, qto, -statMalus * CONT_HIST_1PLY_WEIGHT);
                                }
                                if (contHist2ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist2ply)->update(qpt, qto, -statMalus * CONT_HIST_2PLY_WEIGHT);
                                }
                                if (contHist3ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist3ply)->update(qpt, qto, -statMalus * CONT_HIST_3PLY_WEIGHT);
                                }
                                if (contHist4ply) {
                                    const_cast<ContinuationHistoryEntry*>(contHist4ply)->update(qpt, qto, -statMalus * CONT_HIST_4PLY_WEIGHT);
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

                {
                    int smallBonus = std::min(SearchParams::stat_bonus(depth) / 4, 600);

                    if (!isCapture) {
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
            if (ss->excludedMove != MOVE_NONE) {
                return alpha;
            } else {
                return 0;
            }
        }
    }

    if (moveCount > 0 && bestScore == -VALUE_INFINITE) {
        return alpha;
    }

    Bound bound = bestScore >= beta  ? BOUND_LOWER :
                  bestScore > origAlpha ? BOUND_EXACT : BOUND_UPPER;

    if (!stopped && tte) {
        tte->save(board.key(), score_to_tt(bestScore, ply), staticEval,
                  bound, depth, bestMove, TT.generation(), pvNode);
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

    const U64 nodeMask = (limits.nodes > 0 && limits.nodes < 16384) ? 255 : 16383;
    if (UNLIKELY((searchStats.nodes & nodeMask) == 0)) {
        check_time();
    }

    if (UNLIKELY(stopped)) return 0;

    int ply = board.game_ply() - rootPly;

    if (UNLIKELY(ply > 0 && board.is_draw(ply))) {
        return 0;
    }

    if (UNLIKELY(ply >= MAX_PLY)) {
        return evaluate(board);
    }

    if (UNLIKELY(qsDepth <= -32)) {
        return board.in_check() ? 0 : evaluate(board);
    }

    TT.prefetch(board.key());

    pvLines[ply].clear();

    bool inCheck = board.in_check();

    int legalMoveCount = 0;

    // TT probe BEFORE evaluation — allows TT cutoff to skip expensive eval
    bool ttHit = false;
    TTEntry* tte = TT.probe(board.key(), ttHit);

    Move ttMoves[3];
    int ttMoveCount = 0;
    if (ttHit) {
        TT.get_moves(board.key(), ttMoves, ttMoveCount);
    }

    Move ttMove = (ttMoveCount > 0) ? ttMoves[0] : MOVE_NONE;

    if (ttMove != MOVE_NONE && (!MoveGen::is_pseudo_legal(board, ttMove) || !MoveGen::is_legal(board, ttMove))) {
        ttMove = MOVE_NONE;
        ttHit = false;
        if (ttMoveCount > 0) ttMoves[0] = MOVE_NONE;
    }

    // TT cutoff in qsearch
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

    int staticEval;
    if (inCheck) {
        staticEval = -VALUE_INFINITE;
    } else {
        int rawEval = (ttHit && tte->eval() != VALUE_NONE) ? tte->eval() : evaluate(board, alpha, beta);
        staticEval = rawEval + corrHistory.get(board.side_to_move(), board.pawn_key());
        if (ttHit) {
            int ttScore = score_from_tt(tte->score(), ply);
            Bound ttBound = tte->bound();
            if (ttScore != VALUE_NONE && std::abs(ttScore) < VALUE_MATE_IN_MAX_PLY) {
                if (ttBound == BOUND_EXACT ||
                    (ttBound == BOUND_LOWER && ttScore > staticEval) ||
                    (ttBound == BOUND_UPPER && ttScore < staticEval)) {
                    staticEval = ttScore;
                }
            }
        }
    }

    if (LIKELY(!inCheck)) {
        if (LIKELY(staticEval >= beta)) {
            return staticEval;
        }
        if (staticEval > alpha) {
            alpha = staticEval;
        }
    }

    MoveList moves;

    if (inCheck) {
        MoveGen::generate_evasions(board, moves);
    }

    int bestScore = inCheck ? -VALUE_INFINITE : staticEval;
    Move m;

    bool enemyBareKingInQs = false;
    if (!inCheck) {
        Color enemy = ~board.side_to_move();
        enemyBareKingInQs = (board.pieces(enemy) == square_bb(board.king_square(enemy)));
    }

    if (inCheck) {
        for (size_t i = 0; i < moves.size(); ++i) {
            m = moves[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            ++legalMoveCount;

            StateInfo si;
            board.do_move(m, si);

            int score = -qsearch(board, -beta, -alpha, qsDepth - 1, SQ_NONE);

            board.undo_move(m);

            if (stopped) return 0;

        if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (ply + 1 < MAX_PLY) pvLines[ply].update(m, pvLines[ply + 1]);

                    if (score >= beta) {
                        return score;
                    }
                    alpha = score;
                }
            }
        }
    } else {
        // Qsearch futility pruning: if static eval + max possible capture value
        // cannot raise alpha, skip the entire qsearch and return static eval.
        int maxCaptureValue = 0;
        Color stm = board.side_to_move();
        Color enemy = (stm == WHITE) ? BLACK : WHITE;
        Bitboard enemyPieces = board.pieces(enemy);
        while (enemyPieces) {
            Square sq = pop_lsb(enemyPieces);
            PieceType pt = type_of(board.piece_on(sq));
            if (pt != KING && PieceValue[pt] > maxCaptureValue) {
                maxCaptureValue = PieceValue[pt];
            }
        }
        if (!enemyBareKingInQs && qsDepth < QSEARCH_CHECK_DEPTH &&
            staticEval + maxCaptureValue + QSEARCH_FUTILITY_MARGIN < alpha) {
            return staticEval;
        }

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

            bool captureGivesCheck = mp.hasMoveInfo ? mp.givesCheckInfo : MoveGen::gives_check(board, m);

            // Improved delta pruning: apply to ALL captures including promotions,
            // not just non-queen/rook. If static eval + capture value + margin < alpha,
            // this capture cannot possibly raise alpha, so skip it.
            if (!m.is_promotion() && !captureGivesCheck) {
                if (staticEval + captureValue + DELTA_PRUNING_MARGIN < alpha) {
                    continue;
                }
            } else if (m.is_promotion() && !captureGivesCheck) {
                // Promotion delta pruning: knight promotion worth ~320
                int promoValue = m.promotion_type() == QUEEN ? PieceValue[QUEEN] : PieceValue[KNIGHT];
                if (staticEval + promoValue + DELTA_PRUNING_MARGIN < alpha) {
                    continue;
                }
            }

            if (!m.is_promotion()) {
                bool seeOk = mp.hasSeeZero ? mp.seeZeroInfo : SEE::see_ge(board, m, 0);
                if (!seeOk) {
                    if (!captureGivesCheck || (qsDepth < 0 && !enemyBareKingInQs && !SEE::see_ge(board, m, -250))) {
                        continue;
                    }
                }
            }

            StateInfo si;
            board.do_move(m, si);

            TT.prefetch(board.key());

            bool isRecapture = (recaptureSquare != SQ_NONE && m.to() == recaptureSquare);
            bool isCapture = (capturedPt != NO_PIECE_TYPE) || m.is_enpassant();

            int newQsDepth = (isRecapture && qsDepth > -4) ? qsDepth : qsDepth - 1;
            Square newRecaptureSquare = isCapture ? m.to() : SQ_NONE;

            int score = -qsearch(board, -beta, -alpha, newQsDepth, newRecaptureSquare);

            board.undo_move(m);

            if (stopped) return 0;

        if (score > bestScore) {
                bestScore = score;

                if (score > alpha) {
                    if (ply + 1 < MAX_PLY) pvLines[ply].update(m, pvLines[ply + 1]);

                    if (score >= beta) {
                        return score;
                    }
                    alpha = score;
                }
            }
        }
    }

    int minCheckQsDepth = (enemyBareKingInQs || popcount(board.pieces()) <= 6) ? -2 : QSEARCH_CHECK_DEPTH;
    if (!inCheck && qsDepth >= minCheckQsDepth) {
        MoveList quietChecks;
        MoveGen::generate_checking_moves(board, quietChecks);
        for (size_t i = 0; i < quietChecks.size(); ++i) {
            m = quietChecks[i].move;

            if (!MoveGen::is_legal(board, m)) {
                continue;
            }

            // At first ply of qsearch (qsDepth >= 0), search all legal quiet checks so sacrificial
            // checkmates (e.g. Rh6# / Qg2#) are not pruned. At deeper plies, prune only heavy blunders.
            if (qsDepth < 0 && !SEE::see_ge(board, m, -PieceValue[KNIGHT])) {
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
                    if (ply + 1 < MAX_PLY) pvLines[ply].update(m, pvLines[ply + 1]);

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

void Search::report_info(Board& board, int depth, int score, const PVLine& pv, int multiPVIdx) {
    if (silentMode) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    U64 elapsed = static_cast<U64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );

    if (elapsed == 0) elapsed = 1;

    U64 totalNodes = (Threads.thread_count() > 1) ? Threads.total_nodes() : searchStats.nodes;
    int maxSelDepth = (Threads.thread_count() > 1) ? std::max(searchStats.selDepth, Threads.max_sel_depth()) : searchStats.selDepth;
    U64 nps = totalNodes * 1000 / elapsed;

    std::cout << "info";
    std::cout << " depth " << depth;
    std::cout << " seldepth " << maxSelDepth;

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

    std::cout << " nodes " << totalNodes;
    std::cout << " nps " << nps;
    std::cout << " time " << elapsed;
    std::cout << " hashfull " << TT.hashfull();

    std::cout << " pv";
    Board tempBoard = board;
    StateInfo pvStates[MAX_PLY];
    int movesOutput = 0;
    for (int i = 0; i < pv.length && i < MAX_PLY; ++i) {
        Move m = pv.moves[i];
        if (m == MOVE_NONE) break;

        if (!MoveGen::is_pseudo_legal(tempBoard, m) || !MoveGen::is_legal(tempBoard, m)) {
            break;
        }

        std::cout << " " << move_to_string(m);
        movesOutput++;

        tempBoard.do_move(m, pvStates[i]);
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
