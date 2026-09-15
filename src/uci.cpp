#include "uci.hpp"
#include "movegen.hpp"
#include "tt.hpp"
#include "thread.hpp"
#include "profiler.hpp"
#include "moveorder.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include "tuning.hpp"


namespace UCI {

EngineOptions options;

const std::string ENGINE_NAME = "GC-Engine";
const std::string ENGINE_AUTHOR = "Dhani";
const std::string ENGINE_VERSION = "1.2.1";

static StateInfo stateInfoStack[512];
static int stateStackIdx = 0;

UCIHandler::UCIHandler() : searching(false) {
    stateStackIdx = 0;
    board.set(Board::StartFEN, &stateInfoStack[stateStackIdx]);
    stateStackIdx++;
}

UCIHandler::~UCIHandler() {
    wait_for_search();
}

void UCIHandler::loop() {
    std::string line, token;

    try {
        while (std::getline(std::cin, line)) {
            std::istringstream is(line);
            is >> std::skipws >> token;

            if (token.empty()) continue;

            if (token == "uci") {
                cmd_uci();
            } else if (token == "isready") {
                cmd_isready();
            } else if (token == "ucinewgame") {
                cmd_ucinewgame();
            } else if (token == "position") {
                cmd_position(is);
            } else if (token == "go") {
                cmd_go(is);
            } else if (token == "stop") {
                cmd_stop();
            } else if (token == "quit") {
                cmd_quit();
                break;
            } else if (token == "setoption") {
                cmd_setoption(is);
            } else if (token == "perft") {
                cmd_perft(is);
            } else if (token == "divide") {
                cmd_divide(is);
            } else if (token == "d") {
                cmd_d();
            } else if (token == "eval") {
                cmd_eval();
            } else if (token == "ponderhit") {
                cmd_ponderhit();
            } else if (token == "bench") {
                cmd_bench(is);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Engine Error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Engine Error: Unknown exception" << std::endl;
    }
}

void UCIHandler::cmd_uci() {
    std::cout << "id name " << ENGINE_NAME << " " << ENGINE_VERSION << std::endl;
    std::cout << "id author " << ENGINE_AUTHOR << std::endl;
    std::cout << std::endl;

    std::cout << "option name Hash type spin default " << options.hash << " min 1 max 4096" << std::endl;
    std::cout << "option name Table Memory type spin default 64 min 1 max 1024" << std::endl;
    std::cout << "option name Threads type spin default 1 min 1 max 128" << std::endl;
    std::cout << "option name MultiPV type spin default 1 min 1 max 500" << std::endl;
    std::cout << "option name Ponder type check default true" << std::endl;
    std::cout << "option name Move Overhead type spin default 50 min 0 max 1000" << std::endl;
    std::cout << "option name OwnBook type check default true" << std::endl;
    std::cout << "option name Book File type string default book.bin" << std::endl;
    std::cout << "option name SyzygyPath type string default <empty>" << std::endl;

    std::cout << "option name Contempt type spin default 20 min -100 max 100" << std::endl;
    std::cout << "option name Dynamic Contempt type check default true" << std::endl;

    std::cout << "option name PawnValueMG type spin default " << Tuning::PawnValue.mg << " min 0 max 2000" << std::endl;
    std::cout << "option name PawnValueEG type spin default " << Tuning::PawnValue.eg << " min 0 max 2000" << std::endl;
    std::cout << "option name KnightValueMG type spin default " << Tuning::KnightValue.mg << " min 0 max 2000" << std::endl;
    std::cout << "option name KnightValueEG type spin default " << Tuning::KnightValue.eg << " min 0 max 2000" << std::endl;
    std::cout << "option name BishopValueMG type spin default " << Tuning::BishopValue.mg << " min 0 max 2000" << std::endl;
    std::cout << "option name BishopValueEG type spin default " << Tuning::BishopValue.eg << " min 0 max 2000" << std::endl;
    std::cout << "option name RookValueMG type spin default " << Tuning::RookValue.mg << " min 0 max 2000" << std::endl;
    std::cout << "option name RookValueEG type spin default " << Tuning::RookValue.eg << " min 0 max 2000" << std::endl;
    std::cout << "option name QueenValueMG type spin default " << Tuning::QueenValue.mg << " min 0 max 5000" << std::endl;
    std::cout << "option name QueenValueEG type spin default " << Tuning::QueenValue.eg << " min 0 max 5000" << std::endl;
    std::cout << "option name RookOpenFileBonusMG type spin default " << Tuning::RookOpenFileBonus.mg << " min 0 max 500" << std::endl;
    std::cout << "option name RookOpenFileBonusEG type spin default " << Tuning::RookOpenFileBonus.eg << " min 0 max 500" << std::endl;
    std::cout << "option name KingSafetyWeight type spin default " << Tuning::KingSafetyWeight << " min 0 max 200" << std::endl;
    std::cout << std::endl;

    std::cout << "uciok" << std::endl;
    std::cout.flush();
}

void UCIHandler::cmd_isready() {
    std::cout << "readyok" << std::endl;
    std::cout.flush();
}

void UCIHandler::cmd_ucinewgame() {
    wait_for_search();
    TT.clear();
    stateStackIdx = 0;
    board.set(Board::StartFEN, &stateInfoStack[stateStackIdx]);
    stateStackIdx++;
    Searcher.clear_history();
    Threads.clear_all_history();
}

void UCIHandler::cmd_position(std::istringstream& is) {
    wait_for_search();

    std::string token;
    is >> token;

    stateStackIdx = 0;

    if (token == "startpos") {
        board.set(Board::StartFEN, &stateInfoStack[stateStackIdx]);
        stateStackIdx++;
        is >> token;
    } else if (token == "fen") {
        std::string fen;
        while (is >> token && token != "moves") {
            fen += token + " ";
        }
        board.set(fen, &stateInfoStack[stateStackIdx]);
        stateStackIdx++;
    }

    if (token == "moves") {
        parse_moves(is);
    }
}

void UCIHandler::parse_moves(std::istringstream& is) {
    std::string token;
    while (is >> token) {
        Move m = string_to_move(token);

        if (m != MOVE_NONE) {
            Square from = m.from();
            Square to = m.to();
            Piece pc = board.piece_on(from);

            if (pc == NO_PIECE) {
                continue;
            }
            if (type_of(pc) == KING) {
                if (std::abs(file_of(from) - file_of(to)) > 1) {
                    m = Move::make_castling(from, to);
                }
            } else if (type_of(pc) == PAWN) {
                if (to == board.en_passant_square()) {
                    m = Move::make_enpassant(from, to);
                }
            }

            if (MoveGen::is_legal(board, m) && stateStackIdx < 511) {
                board.do_move(m, stateInfoStack[stateStackIdx++]);
            }
        }
    }
}

void UCIHandler::cmd_go(std::istringstream& is) {
    wait_for_search();

    SearchLimits limits;
    std::string token;

    int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0;

    while (is >> token) {
        if (token == "wtime") {
            is >> wtime;
        } else if (token == "btime") {
            is >> btime;
        } else if (token == "winc") {
            is >> winc;
        } else if (token == "binc") {
            is >> binc;
        } else if (token == "movestogo") {
            is >> movestogo;
        } else if (token == "depth") {
            is >> limits.depth;
        } else if (token == "nodes") {
            is >> limits.nodes;
        } else if (token == "movetime") {
            is >> limits.movetime;
        } else if (token == "infinite") {
            limits.infinite = true;
        } else if (token == "ponder") {
            limits.ponder = true;
        }
    }

    Color us = board.side_to_move();
    int timeLeft = (us == WHITE) ? wtime : btime;
    int increment = (us == WHITE) ? winc : binc;
    if (timeLeft > 0 && !limits.infinite && limits.movetime == 0) {
        limits.time[us] = timeLeft;
        limits.inc[us] = increment;
        limits.movestogo = movestogo;
    }

    start_search(limits);
}

void UCIHandler::start_search(const SearchLimits& limits) {
    searching = true;
    isPondering = limits.ponder;

    std::string searchFen = board.fen();

    Searcher.set_pondering(limits.ponder);

    if (limits.ponder) {
        options.ponderAttempts++;
    }
    searchThread = std::thread([this, limits, searchFen]() {
        StateInfo searchSi;
        Board searchBoard;
        searchBoard.set(searchFen, &searchSi);

        Searcher.start(searchBoard, limits);

        Move bestMove = Searcher.best_move();

        StateInfo validationSi;
        Board validationBoard;
        validationBoard.set(searchFen, &validationSi);

        MoveList legalMoves;
        MoveGen::generate_legal(validationBoard, legalMoves);

        bool moveFound = false;
        for (size_t i = 0; i < legalMoves.size(); ++i) {
            Move legalMove = legalMoves[i].move;

            if (legalMove.from() == bestMove.from() &&
                legalMove.to() == bestMove.to()) {

                if (legalMove.is_promotion()) {
                    if (bestMove.is_promotion()) {
                        if (legalMove.promotion_type() == bestMove.promotion_type()) {
                            bestMove = legalMove;
                            moveFound = true;
                            break;
                        }
                    } else {
                        if (legalMove.promotion_type() == QUEEN) {
                            bestMove = legalMove;
                            moveFound = true;
                            break;
                        }
                    }
                } else {
                    bestMove = legalMove;
                    moveFound = true;
                    break;
                }
            }
        }

        if (bestMove == MOVE_NONE || !moveFound) {
            if (!legalMoves.empty()) {
                bestMove = legalMoves[0].move;
            } else {
                bestMove = MOVE_NONE;
            }
        }

        std::cout << "bestmove " << move_to_string(bestMove);

        Move ponderMove = Searcher.ponder_move();
        if (ponderMove != MOVE_NONE && bestMove != MOVE_NONE && options.ponder) {
            StateInfo si;
            validationBoard.do_move(bestMove, si);

            if (MoveGen::is_pseudo_legal(validationBoard, ponderMove) &&
                MoveGen::is_legal(validationBoard, ponderMove)) {
                std::cout << " ponder " << move_to_string(ponderMove);

                expectedPonderMove = ponderMove;
                ponderFen = validationBoard.fen();
                options.lastPonderMove = ponderMove;
            } else {
                expectedPonderMove = MOVE_NONE;
                ponderFen = "";
            }
        } else {
            expectedPonderMove = MOVE_NONE;
            ponderFen = "";
        }

        std::cout << std::endl;
        std::cout.flush();

        isPondering = false;
        Searcher.set_pondering(false);
        searching = false;
    });
}

void UCIHandler::wait_for_search() {
    if (searchThread.joinable()) {
        Searcher.stop();
        searchThread.join();
    }
}

void UCIHandler::cmd_stop() {
    isPondering = false;
    Searcher.set_pondering(false);
    Searcher.stop();
    wait_for_search();
}

void UCIHandler::cmd_ponderhit() {
    if (isPondering && Searcher.is_pondering()) {
        options.ponderHits++;
        Searcher.on_ponderhit();
        isPondering = false;
    }
}

void UCIHandler::cmd_quit() {
    cmd_stop();
}

void UCIHandler::cmd_setoption(std::istringstream& is) {
    std::string token, name, value;

    is >> token;

    while (is >> token && token != "value") {
        name += (name.empty() ? "" : " ") + token;
    }

    while (is >> token) {
        value += (value.empty() ? "" : " ") + token;
    }
    if (name == "Hash") {
        options.hash = std::stoi(value);
        TT.resize(options.hash);
    } else if (name == "Threads") {
        options.threads = std::stoi(value);
        Threads.set_thread_count(options.threads);
    } else if (name == "MultiPV") {
        options.multiPV = std::stoi(value);
    } else if (name == "Ponder") {
        options.ponder = (value == "true");
    } else if (name == "Move Overhead") {
        options.moveOverhead = std::stoi(value);
    } else if (name == "Book File") {
        options.bookPath = value;
        Book::book.load(value);
    } else if (name == "SyzygyPath") {
        options.syzygyPath = value;
        Tablebase::TB.init(value);
    }
    else if (name == "PawnValueMG") Tuning::PawnValue.mg = std::stoi(value);
    else if (name == "PawnValueEG") Tuning::PawnValue.eg = std::stoi(value);
    else if (name == "KnightValueMG") Tuning::KnightValue.mg = std::stoi(value);
    else if (name == "KnightValueEG") Tuning::KnightValue.eg = std::stoi(value);
    else if (name == "BishopValueMG") Tuning::BishopValue.mg = std::stoi(value);
    else if (name == "BishopValueEG") Tuning::BishopValue.eg = std::stoi(value);
    else if (name == "RookValueMG") Tuning::RookValue.mg = std::stoi(value);
    else if (name == "RookValueEG") Tuning::RookValue.eg = std::stoi(value);
    else if (name == "QueenValueMG") Tuning::QueenValue.mg = std::stoi(value);
    else if (name == "QueenValueEG") Tuning::QueenValue.eg = std::stoi(value);
    else if (name == "RookOpenFileBonusMG") Tuning::RookOpenFileBonus.mg = std::stoi(value);
    else if (name == "RookOpenFileBonusEG") Tuning::RookOpenFileBonus.eg = std::stoi(value);
    else if (name == "KingSafetyWeight") Tuning::KingSafetyWeight = std::stoi(value);
    else if (name == "Contempt") options.contempt = std::stoi(value);
    else if (name == "Dynamic Contempt") options.dynamicContempt = (value == "true");
}

void UCIHandler::cmd_perft(std::istringstream& is) {
    int depth = 6;
    is >> depth;

    std::function<U64(Board&, int)> perft = [&](Board& b, int d) -> U64 {
        if (d == 0) return 1;

        U64 nodes = 0;
        MoveList moves;
        MoveGen::generate_all(b, moves);

        for (int i = 0; i < moves.size(); ++i) {
            Move m = moves[i].move;
            if (!MoveGen::is_legal(b, m)) continue;

            StateInfo si;
            b.do_move(m, si);
            nodes += perft(b, d - 1);
            b.undo_move(m);
        }

        return nodes;
    };

    auto start = std::chrono::steady_clock::now();
    U64 nodes = perft(board, depth);
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    U64 nps = elapsed > 0 ? nodes * 1000 / elapsed : nodes;

    std::cout << "Nodes: " << nodes << std::endl;
    std::cout << "Time: " << elapsed << " ms" << std::endl;
    std::cout << "NPS: " << nps << std::endl;
}

void UCIHandler::cmd_divide(std::istringstream& is) {
    int depth;
    is >> depth;

    std::cout << "Divide depth " << depth << std::endl;

    auto start = std::chrono::steady_clock::now();
    U64 totalNodes = 0;

    MoveList moves;
    MoveGen::generate_all(board, moves);

    std::function<U64(Board&, int)> perft_recursive = [&](Board& b, int d) -> U64 {
        if (d == 0) return 1;
        U64 nodes = 0;
        MoveList mvs;
        MoveGen::generate_all(b, mvs);
        for (int i = 0; i < mvs.size(); ++i) {
            Move m = mvs[i].move;
            if (!MoveGen::is_legal(b, m)) continue;
            StateInfo si;
            b.do_move(m, si);
            nodes += perft_recursive(b, d - 1);
            b.undo_move(m);
        }
        return nodes;
    };

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i].move;
        if (!MoveGen::is_legal(board, m)) continue;

        StateInfo si;
        board.do_move(m, si);
        U64 nodes = perft_recursive(board, depth - 1);
        board.undo_move(m);

        std::cout << move_to_string(m) << ": " << nodes << std::endl;
        totalNodes += nodes;
    }

    auto end = std::chrono::steady_clock::now();
    std::cout << "\nNodes: " << totalNodes << std::endl;
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " ms" << std::endl;
}

void UCIHandler::cmd_d() {
    std::cout << board.pretty();
    std::cout << std::endl;
    std::cout << "FEN: " << board.fen() << std::endl;
    std::cout << "Key: " << std::hex << board.key() << std::dec << std::endl;
}

void UCIHandler::cmd_eval() {
    int score = Searcher.evaluate(board);
    std::cout << "Evaluation: " << score << " cp" << std::endl;
    std::cout << "Side to move: " << (board.side_to_move() == WHITE ? "White" : "Black") << std::endl;
}

void UCIHandler::cmd_bench(std::istringstream& is) {
    int depth = 13;
    int numThreads = 1;
    int hashMB = 16;
    std::string token;
    if (is >> token) depth = std::stoi(token);
    if (is >> token) numThreads = std::stoi(token);
    if (is >> token) hashMB = std::stoi(token);

    depth = std::max(1, std::min(depth, 40));
    numThreads = std::max(1, std::min(numThreads, 128));
    hashMB = std::max(1, std::min(hashMB, 4096));
    int oldHash = options.hash;
    int oldThreads = options.threads;
    TT.resize(hashMB);
    Threads.set_thread_count(numThreads);

    std::cout << "\n===============================================" << std::endl;
    std::cout << "     GC-Engine Benchmark" << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout << "Depth: " << depth << std::endl;
    std::cout << "Threads: " << numThreads << std::endl;
    std::cout << "Hash: " << hashMB << " MB" << std::endl;
    std::cout << "===============================================\n" << std::endl;

    const char* positions[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 2 4",
        "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4",
        "r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
        "8/8/4k3/3p4/3P1K2/8/8/8 w - - 0 1",
        "r1bqr1k1/pp1nbppp/2p2n2/3p2B1/3P4/2NBP3/PPQ1NPPP/R3K2R w KQ - 3 10",
        "r1bq1rk1/ppppbppp/2n2n2/4p3/2P5/5NP1/PP1PPPBP/RNBQ1RK1 w - - 5 6"
    };

    const int numPositions = sizeof(positions) / sizeof(positions[0]);

    U64 totalNodes = 0;
    U64 totalTbHits = 0;
    int totalSelDepth = 0;
    int maxSelDepth = 0;

    auto startTotal = std::chrono::steady_clock::now();

    TT.clear();
    Searcher.clear_history();
    Threads.clear_all_history();

    for (int i = 0; i < numPositions; ++i) {
        StateInfo si;
        Board benchBoard;
        benchBoard.set(positions[i], &si);

        std::cout << "Position " << (i + 1) << "/" << numPositions;
        std::cout << ": " << positions[i] << std::endl;

        SearchLimits benchLimits;
        benchLimits.depth = depth;
        benchLimits.nodes = 100000000;

        auto posStart = std::chrono::steady_clock::now();
        Searcher.start(benchBoard, benchLimits);
        auto posEnd = std::chrono::steady_clock::now();

        const SearchStats& stats = Searcher.stats();

        auto posTime = std::chrono::duration_cast<std::chrono::milliseconds>(posEnd - posStart).count();
        U64 posNps = posTime > 0 ? stats.nodes * 1000 / posTime : stats.nodes;

        std::cout << "  Nodes: " << stats.nodes
                  << " | Time: " << posTime << "ms"
                  << " | NPS: " << posNps
                  << " | SelDepth: " << stats.selDepth << std::endl;
        std::cout << std::endl;

        totalNodes += stats.nodes;
        totalTbHits += stats.tbHits;
        totalSelDepth += stats.selDepth;
        maxSelDepth = std::max(maxSelDepth, stats.selDepth);
    }

    auto endTotal = std::chrono::steady_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count();
    U64 avgNps = totalTime > 0 ? totalNodes * 1000 / totalTime : totalNodes;
    double avgSelDepth = static_cast<double>(totalSelDepth) / numPositions;

    std::cout << "===============================================" << std::endl;
    std::cout << "                  RESULTS" << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout << "Total Nodes   : " << totalNodes << std::endl;
    std::cout << "Total Time    : " << totalTime << " ms" << std::endl;
    std::cout << "Nodes/Second  : " << avgNps << std::endl;
    std::cout << "TB Hits       : " << totalTbHits << std::endl;
    std::cout << "Avg SelDepth  : " << std::fixed << std::setprecision(1) << avgSelDepth << std::endl;
    std::cout << "Max SelDepth  : " << maxSelDepth << std::endl;
    std::cout << "Positions     : " << numPositions << std::endl;
    std::cout << "===============================================" << std::endl;
    std::cout << std::endl;

    std::cout << totalNodes << " nodes " << avgNps << " nps" << std::endl;

    Profiler::print_results();
    ProfilerAnalysis::analyze_bottlenecks();
    TT.resize(oldHash);
    Threads.set_thread_count(oldThreads);
}

}
