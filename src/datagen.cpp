#include "datagen.hpp"
#include "board.hpp"
#include "bitboard.hpp"
#include "movegen.hpp"
#include "search.hpp"
#include "eval.hpp"
#include "nnue.hpp"
#include "types.hpp"
#include "tt.hpp"
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <cmath>

namespace Datagen {

struct PositionEntry {
    std::string fen;
    int whiteScore;
    int pieceCount;
};

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string trim_copy(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parse_seed_or_relabel_line(const std::string& raw, std::string& outFen, double& outWdl, bool& hasWdl) {
    std::string line = trim_copy(raw);
    if (line.empty() || line[0] == '#') return false;

    hasWdl = false;
    outWdl = 0.5;
    size_t p1 = line.find('|');
    if (p1 != std::string::npos) {
        outFen = trim_copy(line.substr(0, p1));
        size_t p2 = line.find('|', p1 + 1);
        std::string wdlTok = (p2 != std::string::npos) ? trim_copy(line.substr(p2 + 1)) : trim_copy(line.substr(p1 + 1));
        if (!wdlTok.empty()) {
            try {
                outWdl = std::stod(wdlTok);
                hasWdl = true;
            } catch (...) {}
        }
        return !outFen.empty();
    }

    if (line.find("\"1-0\"") != std::string::npos) {
        outWdl = 1.0;
        hasWdl = true;
    } else if (line.find("\"0-1\"") != std::string::npos) {
        outWdl = 0.0;
        hasWdl = true;
    } else if (line.find("\"1/2-1/2\"") != std::string::npos) {
        outWdl = 0.5;
        hasWdl = true;
    }

    // Extract 4 or 6 FEN tokens before EPD opcodes (e.g., bm/c9/id)
    std::istringstream iss(line);
    std::string t0, t1, t2, t3, t4, t5;
    if (!(iss >> t0 >> t1 >> t2 >> t3)) return false;
    outFen = t0 + " " + t1 + " " + t2 + " " + t3;
    if ((iss >> t4) && std::all_of(t4.begin(), t4.end(), ::isdigit) &&
        (iss >> t5) && std::all_of(t5.begin(), t5.end(), ::isdigit)) {
        outFen += " " + t4 + " " + t5;
    } else {
        outFen += " 0 1";
    }
    return true;
}

static std::vector<std::string> load_fen_seeds(const std::string& path) {
    std::vector<std::string> fens;
    if (path.empty()) return fens;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::string fen;
        double wdl = 0.5;
        bool hasWdl = false;
        if (parse_seed_or_relabel_line(line, fen, wdl, hasWdl)) {
            fens.push_back(std::move(fen));
        }
    }
    return fens;
}

// Plays a single game and returns vector of quiet position entries with their final WDL
bool play_game(const Config& config, std::mt19937_64& rng, Search& searcher,
               const std::vector<std::string>& seedFens,
               std::vector<PositionEntry>& outPositions, double& outWdl) {
    outPositions.clear();
    outWdl = 0.5;

    std::vector<StateInfo> stateHistory(1024);
    Board board;

    // 1. Opening Setup: either seed FEN (+0..2 random plies) or 8/10 random plies from StartFEN
    bool validOpening = false;
    int seedAttempts = 0;
    while (!validOpening) {
        if (!seedFens.empty() && seedAttempts < 64) {
            ++seedAttempts;
            std::uniform_int_distribution<size_t> fdist(0, seedFens.size() - 1);
            board.set(seedFens[fdist(rng)], &stateHistory[0]);

            int extraPlies = std::uniform_int_distribution<int>(0, 2)(rng);
            bool seedFailed = false;
            for (int ply = 0; ply < extraPlies; ++ply) {
                MoveList lm;
                MoveGen::generate_legal(board, lm);
                if (lm.empty()) {
                    seedFailed = true;
                    break;
                }
                std::uniform_int_distribution<size_t> mdist(0, lm.size() - 1);
                board.do_move(lm[mdist(rng)].move, stateHistory[ply + 1]);
            }
            if (seedFailed) continue;

            MoveList testMoves;
            MoveGen::generate_legal(board, testMoves);
            if (testMoves.empty() || board.in_check() || board.is_draw(0)) continue;
            int seedPc = popcount(board.pieces());
            if (seedPc < config.minPieces) continue;

            int eval = Eval::evaluate(board);
            int maxSeedEval = (seedPc <= 6) ? 2500 : 1500;
            if (std::abs(eval) > maxSeedEval) continue;
            validOpening = true;
            break;
        }

        board.set(Board::StartFEN, &stateHistory[0]);
        std::uniform_int_distribution<int> coin(0, 1);
        int openingPlies = coin(rng) ? 8 : 10;

        bool openingFailed = false;
        for (int ply = 0; ply < openingPlies; ++ply) {
            MoveList legalMoves;
            MoveGen::generate_legal(board, legalMoves);
            if (legalMoves.empty()) {
                openingFailed = true;
                break;
            }
            std::uniform_int_distribution<size_t> dist(0, legalMoves.size() - 1);
            Move randomMove = legalMoves[dist(rng)].move;
            board.do_move(randomMove, stateHistory[ply + 1]);
        }

        if (openingFailed) continue;

        MoveList testMoves;
        MoveGen::generate_legal(board, testMoves);
        if (testMoves.empty()) continue;

        if (board.in_check()) continue;

        int eval = Eval::evaluate(board);
        if (std::abs(eval) > 600) continue;

        validOpening = true;
    }

    // Reset history tables for a fresh game
    searcher.clear_history();

    // 2. Self-Play Loop
    int winStreakWhite = 0;
    int winStreakBlack = 0;
    int drawStreak = 0;
    double gameResult = -1.0; // -1 = undetermined

    SearchLimits limits;
    if (config.depth > 0) {
        limits.depth = config.depth;
    }
    if (config.nodes > 0) {
        limits.nodes = config.nodes;
    } else if (config.depth == 0) {
        limits.nodes = 5000;
    }

    std::vector<StateInfo> searchHistory(512);

    while (gameResult < 0.0) {
        int ply = board.game_ply();
        if (ply >= 400) {
            gameResult = 0.5;
            break;
        }

        MoveList legalMoves;
        MoveGen::generate_legal(board, legalMoves);
        if (legalMoves.empty()) {
            if (board.in_check()) {
                gameResult = (board.side_to_move() == WHITE) ? 0.0 : 1.0;
            } else {
                gameResult = 0.5;
            }
            break;
        }

        // Run Search on local board copy
        Board searchBoard;
        searchBoard.copy_with_history(board, searchHistory.data(), 512);

        searcher.start(searchBoard, limits);
        Move bestMove = searcher.best_move();
        int bestScore = searcher.best_score();

        if (bestMove == MOVE_NONE) {
            bestMove = legalMoves[0].move;
            bestScore = Eval::evaluate(board);
        }

        int whiteScore = (board.side_to_move() == WHITE) ? bestScore : -bestScore;

        // 3. Quiet Position Filter
        int pieceCount = popcount(board.pieces());
        int minSavePly = seedFens.empty() ? 16 : 0;
        bool quiet = (ply >= minSavePly) &&
                     (!board.in_check()) &&
                     (!board.is_capture(bestMove)) &&
                     (!bestMove.is_promotion()) &&
                     (std::abs(bestScore) <= config.maxScore) &&
                     (pieceCount >= config.minPieces);

        if (quiet) {
            outPositions.push_back({ board.fen(), whiteScore, pieceCount });
        }

        // 4. Execute Move on Board
        if (ply + 1 >= static_cast<int>(stateHistory.size())) {
            stateHistory.resize(stateHistory.size() * 2);
        }
        board.do_move(bestMove, stateHistory[ply + 1]);

        // 5. Adjudication Checks
        int pcAfter = popcount(board.pieces());
        int winThresh = (config.egDelay && pcAfter <= 8) ? std::max(config.winAdjCp, 2500) : config.winAdjCp;

        // Win Adjudication: 4 consecutive plies of |score| >= winThresh
        if (whiteScore >= winThresh) {
            winStreakWhite++;
            winStreakBlack = 0;
            if (winStreakWhite >= 4) {
                gameResult = 1.0;
                break;
            }
        } else if (whiteScore <= -winThresh) {
            winStreakBlack++;
            winStreakWhite = 0;
            if (winStreakBlack >= 4) {
                gameResult = 0.0;
                break;
            }
        } else {
            winStreakWhite = 0;
            winStreakBlack = 0;
        }

        // Draw Adjudication: 8 consecutive plies of |score| <= 15 cp (delayed in low-piece endgames)
        bool allowEarlyDrawAdj = !(config.egDelay && pcAfter <= 8 && board.halfmove_clock() < 40);
        if (allowEarlyDrawAdj && ply >= 80 && std::abs(bestScore) <= 15) {
            drawStreak++;
            if (drawStreak >= 8) {
                gameResult = 0.5;
                break;
            }
        } else {
            drawStreak = 0;
        }

        // Board rule draw: 3-fold repetition / 50-move / insufficient material
        if (board.is_draw(0)) {
            gameResult = 0.5;
            break;
        }
    }

    outWdl = gameResult;
    return true;
}

void run_relabel(const Config& config) {
    if (config.inputPath.empty()) {
        std::cerr << "relabel error: specify 'in <input_file>'" << std::endl;
        return;
    }
    std::ifstream inFile(config.inputPath);
    if (!inFile.is_open()) {
        std::cerr << "relabel error: cannot open input file " << config.inputPath << std::endl;
        return;
    }

    struct InputItem {
        std::string fen;
        double wdl;
        bool hasWdl;
    };
    std::vector<InputItem> items;
    std::string line;
    while (std::getline(inFile, line)) {
        std::string fen;
        double wdl = 0.5;
        bool hasWdl = false;
        if (parse_seed_or_relabel_line(line, fen, wdl, hasWdl)) {
            items.push_back({ std::move(fen), wdl, hasWdl });
            if (config.maxPositions > 0 && static_cast<int>(items.size()) >= config.maxPositions) {
                break;
            }
        }
    }
    inFile.close();

    if (items.empty()) {
        std::cerr << "relabel error: no valid positions loaded from " << config.inputPath << std::endl;
        return;
    }

    std::filesystem::path outPath(config.outputPath);
    if (outPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
    }
    std::ofstream outFile(config.outputPath, std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        std::cerr << "relabel error: cannot open output file " << config.outputPath << std::endl;
        return;
    }

    SearchLimits limits;
    if (config.depth > 0) limits.depth = config.depth;
    if (config.nodes > 0) limits.nodes = config.nodes;
    else if (config.depth == 0) limits.nodes = 3000;

    int numThreads = std::max(1, config.threads);
    std::atomic<size_t> nextIdx{0};
    std::atomic<size_t> doneCount{0};
    std::atomic<size_t> savedCount{0};
    std::mutex fileMutex;
    std::vector<std::thread> workers;

    std::cout << "relabel starting: " << items.size() << " positions, "
              << numThreads << " threads, "
              << (config.staticEval ? "static eval"
                  : (config.depth > 0 ? ("depth " + std::to_string(config.depth)) : ("nodes " + std::to_string(limits.nodes))))
              << (config.minOverlay > 0 ? (", minov " + std::to_string(config.minOverlay)) : "")
              << ", output: " << config.outputPath << std::endl;

    auto startTime = std::chrono::steady_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&]() {
            std::unique_ptr<Search> workerSearch;
            if (!config.staticEval) {
                workerSearch = std::make_unique<Search>();
                workerSearch->set_standalone(true);
            }
            std::vector<StateInfo> st(16);
            std::vector<std::string> localLines;
            localLines.reserve(256);

            auto flushLocal = [&]() {
                if (localLines.empty()) return;
                std::lock_guard<std::mutex> lock(fileMutex);
                for (const auto& l : localLines) {
                    outFile << l << "\n";
                }
                savedCount += localLines.size();
                localLines.clear();
            };

            while (true) {
                size_t idx = nextIdx.fetch_add(1);
                if (idx >= items.size()) break;

                if (workerSearch && idx % 512 == 0) {
                    workerSearch->clear_history();
                }

                Board board;
                board.set(items[idx].fen, &st[0]);
                MoveList legal;
                MoveGen::generate_legal(board, legal);
                if (!legal.empty() && !board.in_check()) {
                    bool keep = true;
                    int bestScore = 0;
                    if (config.staticEval) {
                        bestScore = Eval::evaluate(board);
                        if (config.minOverlay > 0) {
                            bool hasNonKingBoth = (board.pieces(WHITE) & ~board.pieces(WHITE, KING)) &&
                                                  (board.pieces(BLACK) & ~board.pieces(BLACK, KING));
                            if (!hasNonKingBoth || !NNUE::is_loaded()) {
                                keep = false;
                            } else {
                                int raw = NNUE::evaluate(board, board.accumulator()) + 12;
                                if (std::abs(bestScore - raw) < config.minOverlay) {
                                    keep = false;
                                }
                            }
                        }
                    } else {
                        workerSearch->start(board, limits);
                        Move bestMove = workerSearch->best_move();
                        if (bestMove == MOVE_NONE || board.is_capture(bestMove) || bestMove.is_promotion()) {
                            keep = false;
                        } else {
                            bestScore = workerSearch->best_score();
                        }
                    }
                    if (keep) {
                        int whiteScore = (board.side_to_move() == WHITE) ? bestScore : -bestScore;
                        whiteScore = std::clamp(whiteScore, -config.maxScore, config.maxScore);
                        double wdl = items[idx].wdl;
                        if (!items[idx].hasWdl) {
                            wdl = 1.0 / (1.0 + std::exp(-std::clamp(static_cast<double>(whiteScore) / 400.0, -15.0, 15.0)));
                        }
                        std::ostringstream oss;
                        oss << items[idx].fen << " | " << whiteScore << " | "
                            << std::fixed << std::setprecision(2) << wdl;
                        localLines.push_back(oss.str());
                        if (localLines.size() >= 256) {
                            flushLocal();
                        }
                    }
                }

                size_t done = ++doneCount;
                if (done % 10000 == 0 || done == items.size()) {
                    std::cout << "relabel: " << done << "/" << items.size()
                              << " (" << savedCount.load() << " saved)" << std::endl;
                }
            }
            flushLocal();
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    outFile.close();

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    std::cout << "relabel completed: " << savedCount.load() << " positions saved to "
              << config.outputPath << " in " << elapsedMs << " ms ("
              << (elapsedMs > 0 ? savedCount.load() * 1000 / elapsedMs : 0) << " pos/s)" << std::endl;
}

void run(const Config& config) {
    if (config.relabel) {
        run_relabel(config);
        return;
    }

    std::vector<std::string> seedFens = load_fen_seeds(config.fenFile);
    if (!config.fenFile.empty()) {
        std::cout << "datagen: loaded " << seedFens.size() << " seed FENs from " << config.fenFile << std::endl;
    }

    std::filesystem::path outPath(config.outputPath);
    if (outPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
        if (ec) {
            std::cerr << "datagen error: cannot create directories for " << config.outputPath
                      << " (" << ec.message() << ")" << std::endl;
            return;
        }
    }

    std::ofstream outFile(config.outputPath, std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        std::cerr << "datagen error: cannot open output file " << config.outputPath << std::endl;
        return;
    }

    std::mutex fileMutex;
    std::atomic<int> gamesStarted{0};
    std::atomic<int> gamesFinished{0};
    std::atomic<uint64_t> totalPositions{0};
    uint64_t savedMid = 0;
    uint64_t savedPracEg = 0;
    uint64_t savedLateEg = 0;

    int numThreads = std::max(1, config.threads);
    int targetGames = std::max(1, config.games);
    std::vector<std::thread> workers;

    std::cout << "datagen starting: " << targetGames << " games, "
              << numThreads << " threads, "
              << (config.depth > 0 ? ("depth " + std::to_string(config.depth)) : ("nodes " + std::to_string(config.nodes > 0 ? config.nodes : 5000)))
              << ", minpieces " << config.minPieces
              << (config.stratified ? ", stratified (40/40/20)" : "")
              << (config.egDelay ? ", egdelay" : "")
              << ", output: " << config.outputPath << std::endl;

    auto startTime = std::chrono::steady_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, t]() {
            uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count()
                          + static_cast<uint64_t>(t) * 999983ULL;
            std::mt19937_64 rng(seed);
            std::uniform_real_distribution<double> uni(0.0, 1.0);

            auto workerSearch = std::make_unique<Search>();
            workerSearch->set_standalone(true);

            while (true) {
                int gameIdx = gamesStarted.fetch_add(1);
                if (gameIdx >= targetGames) break;

                std::vector<PositionEntry> positions;
                double wdl = 0.5;
                play_game(config, rng, *workerSearch, seedFens, positions, wdl);

                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    uint64_t keptThisGame = 0;
                    for (const auto& entry : positions) {
                        bool isMid = (entry.pieceCount >= 17);
                        bool isPracEg = (entry.pieceCount >= 9 && entry.pieceCount <= 16);
                        if (config.stratified) {
                            uint64_t tot = savedMid + savedPracEg + savedLateEg;
                            if (tot >= 50) {
                                double midRatio = static_cast<double>(savedMid) / static_cast<double>(tot);
                                double pracRatio = static_cast<double>(savedPracEg) / static_cast<double>(tot);
                                if (isMid && midRatio > 0.40) {
                                    double dropProb = std::min(0.85, (midRatio - 0.40) * 4.0);
                                    if (uni(rng) < dropProb) continue;
                                } else if (isPracEg && pracRatio > 0.42) {
                                    double dropProb = std::min(0.75, (pracRatio - 0.42) * 3.0);
                                    if (uni(rng) < dropProb) continue;
                                }
                            }
                        }

                        outFile << entry.fen << " | " << entry.whiteScore << " | "
                                << std::fixed << std::setprecision(1) << wdl << "\n";
                        if (isMid) ++savedMid;
                        else if (isPracEg) ++savedPracEg;
                        else ++savedLateEg;
                        ++keptThisGame;
                    }
                    outFile.flush();
                    totalPositions += keptThisGame;
                }

                int done = ++gamesFinished;
                int interval = (targetGames >= 1000) ? 100 : (targetGames >= 200 ? 50 : 10);
                if (done % interval == 0 || done == targetGames) {
                    std::cout << "datagen: " << done << "/" << targetGames
                              << " games (" << totalPositions.load() << " quiet positions)" << std::endl;
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    outFile.close();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    uint64_t tot = totalPositions.load();
    double pMid = tot > 0 ? (100.0 * savedMid / tot) : 0.0;
    double pPrac = tot > 0 ? (100.0 * savedPracEg / tot) : 0.0;
    double pLate = tot > 0 ? (100.0 * savedLateEg / tot) : 0.0;
    std::cout << "datagen completed: " << targetGames << " games, "
              << tot << " positions saved to "
              << config.outputPath << " in " << elapsedMs << " ms ("
              << (elapsedMs > 0 ? tot * 1000 / elapsedMs : 0) << " pos/s)"
              << std::endl;
    std::cout << std::fixed << std::setprecision(1)
              << "Phase mix: mid(17-32)=" << savedMid << " (" << pMid << "%), "
              << "prac_eg(9-16)=" << savedPracEg << " (" << pPrac << "%), "
              << "late_eg(3-8)=" << savedLateEg << " (" << pLate << "%)"
              << std::endl;
}

void run_cli(std::istringstream& is) {
    Config config;
    std::string token;
    while (is >> token) {
        if (token == "relabel") {
            config.relabel = true;
        } else if (token == "static") {
            config.staticEval = true;
        } else if (token == "stratified") {
            config.stratified = true;
            config.minPieces = 3;
            config.egDelay = true;
        } else if (token == "egdelay") {
            config.egDelay = true;
        } else if (token == "minov") {
            is >> config.minOverlay;
        } else if (token == "games") {
            is >> config.games;
        } else if (token == "threads") {
            is >> config.threads;
        } else if (token == "nodes") {
            is >> config.nodes;
        } else if (token == "depth") {
            is >> config.depth;
        } else if (token == "minpieces") {
            is >> config.minPieces;
        } else if (token == "maxscore") {
            is >> config.maxScore;
        } else if (token == "winadj") {
            is >> config.winAdjCp;
        } else if (token == "maxpos") {
            is >> config.maxPositions;
        } else if (token == "in" || token == "input") {
            is >> config.inputPath;
        } else if (token == "fens") {
            is >> config.fenFile;
        } else if (token == "out" || token == "output") {
            is >> config.outputPath;
        }
    }

    config.inputPath = strip_quotes(config.inputPath);
    config.fenFile = strip_quotes(config.fenFile);
    config.outputPath = strip_quotes(config.outputPath);

    run(config);
}

} // namespace Datagen
