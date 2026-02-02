#include "uci.hpp"
#include "movegen.hpp"
#include "tt.hpp"
#include "thread.hpp"
#include "profiler.hpp"
#include "datagen.hpp"
#include "moveorder.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include "tuning.hpp"


namespace UCI {

EngineOptions options;
TimeManager timeMgr;

const std::string ENGINE_NAME = "GC-Engine";
const std::string ENGINE_AUTHOR = "Dhani";
const std::string ENGINE_VERSION = "1.2";

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
            } else if (token == "datagen") {
                cmd_datagen(is);
            } else if (token == "threats") {
                debug_threats(board);
            } else if (token == "threatmove") {
                std::string moveStr;
                if (is >> moveStr) {
                    Move m = string_to_move(moveStr);
                    debug_move_threat_score(board, m);
                }
            } else if (token == "quietsplit") {
                debug_quiet_split(board);
            } else if (token == "killerstats") {
                g_killerStats.print();
            } else if (token == "clearstats") {
                g_killerStats.clear();
                std::cout << "Killer statistics cleared.\n";
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

    std::cout << "option name Hash type spin default 256 min 1 max 4096" << std::endl;
    std::cout << "option name Table Memory type spin default 64 min 1 max 1024" << std::endl;
    std::cout << "option name Threads type spin default 1 min 1 max 128" << std::endl;
    std::cout << "option name MultiPV type spin default 1 min 1 max 500" << std::endl;
    std::cout << "option name Ponder type check default true" << std::endl;
    std::cout << "option name Move Overhead type spin default 10 min 0 max 5000" << std::endl;
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
        timeMgr.init(us, timeLeft, increment, movestogo, 0);
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

void UCIHandler::cmd_datagen(std::istringstream& is) {
    std::string subcommand;
    is >> subcommand;

    if (subcommand == "stop") {
        if (DataGen::is_running()) {
            DataGen::stop();
            std::cout << "Data generation stopped." << std::endl;
        } else {
            std::cout << "No data generation running." << std::endl;
        }
        return;
    }

    if (subcommand == "status") {
        if (DataGen::is_running()) {
            std::cout << "Data generation is running." << std::endl;
            DataGen::get_stats().print();
        } else {
            std::cout << "No data generation running." << std::endl;
        }
        return;
    }

    if (subcommand == "view") {
        std::string path = "data/training.binpack";
        size_t count = 10;
        size_t offset = 0;

        std::string token;
        while (is >> token) {
            if (token == "file" || token == "path") {
                is >> path;
            } else if (token == "count" || token == "n") {
                is >> count;
            } else if (token == "offset" || token == "skip") {
                is >> offset;
            } else {
                path = token;
            }
        }

        DataGen::view_binpack_file(path, count, offset);
        return;
    }

    if (subcommand == "convert") {
        std::string binpack_path = "data/training.binpack";
        std::string epd_path = "data/training.epd";
        size_t max_entries = 0;

        std::string token;
        while (is >> token) {
            if (token == "input" || token == "binpack") {
                is >> binpack_path;
            } else if (token == "output" || token == "epd") {
                is >> epd_path;
            } else if (token == "max" || token == "limit") {
                is >> max_entries;
            } else {
                binpack_path = token;
            }
        }

        DataGen::convert_to_epd(binpack_path, epd_path, max_entries);
        return;
    }

    if (subcommand == "stats") {
        std::string path = "data/training.binpack";
        std::string token;
        if (is >> token) {
            path = token;
        }

        DataGen::FileStats stats;
        if (DataGen::get_file_stats(path, stats)) {
            std::cout << "\n=== Training Data Statistics ===" << std::endl;
            std::cout << "File: " << path << std::endl;
            std::cout << "Total entries: " << stats.total_entries << std::endl;
            std::cout << "White wins: " << stats.white_wins
                      << " (" << (stats.total_entries > 0 ? stats.white_wins * 100.0 / stats.total_entries : 0) << "%)" << std::endl;
            std::cout << "Black wins: " << stats.black_wins
                      << " (" << (stats.total_entries > 0 ? stats.black_wins * 100.0 / stats.total_entries : 0) << "%)" << std::endl;
            std::cout << "Draws: " << stats.draws
                      << " (" << (stats.total_entries > 0 ? stats.draws * 100.0 / stats.total_entries : 0) << "%)" << std::endl;
            std::cout << "Score range: [" << stats.min_score << ", " << stats.max_score << "]" << std::endl;
            std::cout << "Average score: " << (stats.total_entries > 0 ? stats.total_score / (int64_t)stats.total_entries : 0) << std::endl;
            std::cout << "================================\n" << std::endl;
        } else {
            std::cerr << "Error: Cannot read file " << path << std::endl;
        }
        return;
    }

    if (subcommand == "filter") {
        DataGen::FilterConfig config = DataGen::parse_filter_config(is);

        if (config.input_path.empty()) {
            std::cerr << "Error: No input file specified. Use 'datagen filter input <path>'" << std::endl;
            return;
        }

        DataGen::FilterStats stats;
        if (DataGen::filter_binpack(config, stats)) {
            std::cout << "Filter completed successfully!" << std::endl;
        } else {
            std::cerr << "Filter failed!" << std::endl;
        }
        return;
    }

    if (subcommand == "help" || subcommand == "?") {
        std::cout << "\n=== Data Generation Commands ===" << std::endl;
        std::cout << "datagen start [options]  - Start data generation" << std::endl;
        std::cout << "datagen stop             - Stop data generation" << std::endl;
        std::cout << "datagen status           - Show generation status" << std::endl;
        std::cout << "datagen view [file]      - View binpack file contents" << std::endl;
        std::cout << "datagen stats [file]     - Show file statistics" << std::endl;
        std::cout << "datagen convert [opts]   - Convert binpack to EPD text format" << std::endl;
        std::cout << "datagen filter [opts]    - Filter existing data for quiet positions" << std::endl;

        std::cout << "\nOptions for 'datagen start':" << std::endl;
        std::cout << "  threads <n>      - Number of worker threads (default: 1)" << std::endl;
        std::cout << "  hash <mb>        - Hash table size in MB (default: 16)" << std::endl;
        std::cout << "  depth <n>        - Search depth (default: 8)" << std::endl;
        std::cout << "  nodes <n>        - Node limit per move (default: 5000)" << std::endl;
        std::cout << "  games <n>        - Number of games to play (default: 100000)" << std::endl;
        std::cout << "  random <n>       - Random opening plies (default: 8)" << std::endl;
        std::cout << "  maxply <n>       - Maximum game length (default: 400)" << std::endl;
        std::cout << "  output <path>    - Output file path (default: data/training.binpack)" << std::endl;
        std::cout << "  resign <cp>      - Resign threshold in cp (default: 3000)" << std::endl;
        std::cout << "  book <path>      - Opening book path (default: book/Perfect2023.bin)" << std::endl;
        std::cout << "  bookdepth <n>    - Book depth in half-moves (default: 12)" << std::endl;
        std::cout << "  nobook           - Disable opening book" << std::endl;
        std::cout << "  eval_limit <cp>  - Clamp scores to +/-limit (default: disabled)" << std::endl;
        std::cout << "  qsearch <cp>     - QSearch quietness margin (default: 60)" << std::endl;
        std::cout << "  search_margin <cp>  - Search margin for quiet detection (default: 70)" << std::endl;
        std::cout << "  max_score <cp>   - Skip positions with |score| > max (default: 2500)" << std::endl;

        std::cout << "\nOptions for 'datagen filter':" << std::endl;
        std::cout << "  input <path>     - Input binpack file (required)" << std::endl;
        std::cout << "  output <path>    - Output file path (default: input_filtered.binpack)" << std::endl;
        std::cout << "  qsearch <cp>     - Qsearch margin threshold (default: 60)" << std::endl;
        std::cout << "  max_score <cp>   - Max absolute score to keep (default: 2500)" << std::endl;
        std::cout << "  eval_limit <cp>  - Clamp scores to +/-limit without discarding (default: disabled)" << std::endl;

        std::cout << "\nOptions for 'datagen view':" << std::endl;
        std::cout << "  file <path>      - File to view (default: data/training.binpack)" << std::endl;
        std::cout << "  count <n>        - Number of entries to show (default: 10)" << std::endl;
        std::cout << "  offset <n>       - Starting entry offset (default: 0)" << std::endl;

        std::cout << "\nOptions for 'datagen convert':" << std::endl;
        std::cout << "  input <path>     - Binpack file to convert" << std::endl;
        std::cout << "  output <path>    - EPD output file path" << std::endl;
        std::cout << "  max <n>          - Maximum entries to convert (0 = all)" << std::endl;

        std::cout << "\nExamples:" << std::endl;
        std::cout << "  datagen start threads 8 depth 8 games 1000000" << std::endl;
        std::cout << "  datagen start threads 8 depth 9 qsearch 60 games 500000" << std::endl;
        std::cout << "  datagen start threads 128 hash 8192 games 1000000" << std::endl;
        std::cout << "  datagen start output data/custom.binpack" << std::endl;
        std::cout << "  datagen filter input data/training.binpack qsearch 60" << std::endl;
        std::cout << "  datagen view data/training.binpack count 20" << std::endl;
        std::cout << "  datagen stats data/training.binpack" << std::endl;
        std::cout << "  datagen convert data/training.binpack output data/training.epd" << std::endl;
        std::cout << "================================\n" << std::endl;
        return;
    }

    if (subcommand == "start" || !subcommand.empty()) {
        if (DataGen::is_running()) {
            std::cout << "Data generation already running. Use 'datagen stop' first." << std::endl;
            return;
        }
        std::string remaining;
        if (subcommand != "start") {
            remaining = subcommand + " ";
        }
        std::string token;
        while (is >> token) {
            remaining += token + " ";
        }

        std::istringstream config_stream(remaining);
        DataGen::DataGenConfig config = DataGen::parse_config(config_stream);

        DataGen::start(config);
        std::cout << "Data generation started in background." << std::endl;
        std::cout << "Format: binpack" << std::endl;
        std::cout << "Output: " << config.output << std::endl;
        std::cout << "Use 'datagen status' to check progress, 'datagen stop' to cancel." << std::endl;
        return;
    }

    std::cout << "Unknown datagen command. Use 'datagen help' for usage." << std::endl;
}

TimeManager::TimeManager()
    : optimalTime(0), maximumTime(0), startTime(0),
      incrementTime(0), movesToGo(0), stability(1.0) {}

void TimeManager::init(Color us, int timeLeft, int increment, int mtg, int moveTime) {
    (void)us;

    incrementTime = increment;
    movesToGo = mtg > 0 ? mtg : 40;

    if (moveTime > 0) {
        optimalTime = moveTime - UCI::options.moveOverhead;
        maximumTime = moveTime - UCI::options.moveOverhead;
        return;
    }

    int overhead = UCI::options.moveOverhead;
    int safeTime = std::max(1, timeLeft - overhead);

    int baseTime = safeTime / movesToGo;

    baseTime += increment * 3 / 4;

    optimalTime = std::min(baseTime, safeTime / 2);

    maximumTime = std::min(safeTime * 3 / 4, baseTime * 3);

    optimalTime = std::max(10, optimalTime);
    maximumTime = std::max(50, maximumTime);

    stability = 1.0;
}

bool TimeManager::should_stop(int elapsed, int depth, bool bestMoveStable) {
    if (depth < 1) return false;

    if (elapsed >= maximumTime) return true;

    int adjustedOptimal = static_cast<int>(optimalTime * stability);

    if (bestMoveStable && elapsed >= adjustedOptimal / 2) {
        return true;
    }

    if (elapsed >= adjustedOptimal) {
        return true;
    }

    return false;
}

void TimeManager::adjust(bool scoreDropped, bool bestMoveChanged) {
    if (scoreDropped) {
        stability = std::min(2.0, stability * 1.2);
    }

    if (bestMoveChanged) {
        stability = std::min(2.0, stability * 1.1);
    } else {
        stability = std::max(0.5, stability * 0.95);
    }
}

}
