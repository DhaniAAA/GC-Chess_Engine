#include "datagen.hpp"
#include "board.hpp"
#include "bitboard.hpp"
#include "movegen.hpp"
#include "search.hpp"
#include "eval.hpp"
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
};

// Plays a single game and returns vector of quiet position entries with their final WDL
bool play_game(const Config& config, std::mt19937_64& rng, Search& searcher,
               std::vector<PositionEntry>& outPositions, double& outWdl) {
    outPositions.clear();
    outWdl = 0.5;

    std::vector<StateInfo> stateHistory(1024);
    Board board;

    // 1. Random Opening Exit (8 or 10 random plies, 50%:50% distribution)
    bool validOpening = false;
    while (!validOpening) {
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

        // 3. Quiet Position Filter (All 6 Mandatory Rules)
        // Rule 1: gamePly >= 16
        // Rule 2: !board.in_check()
        // Rule 3: !board.is_capture(bestMove)
        // Rule 4: !bestMove.is_promotion()
        // Rule 5: std::abs(bestScore) <= 3000
        // Rule 6: popcount(board.pieces()) >= 5
        bool quiet = (ply >= 16) &&
                     (!board.in_check()) &&
                     (!board.is_capture(bestMove)) &&
                     (!bestMove.is_promotion()) &&
                     (std::abs(bestScore) <= 3000) &&
                     (popcount(board.pieces()) >= 5);

        if (quiet) {
            outPositions.push_back({ board.fen(), whiteScore });
        }

        // 4. Execute Move on Board
        if (ply + 1 >= static_cast<int>(stateHistory.size())) {
            stateHistory.resize(stateHistory.size() * 2);
        }
        board.do_move(bestMove, stateHistory[ply + 1]);

        // 5. Adjudication Checks
        // Win Adjudication: 4 consecutive plies of |score| >= 1000 cp
        if (whiteScore >= 1000) {
            winStreakWhite++;
            winStreakBlack = 0;
            if (winStreakWhite >= 4) {
                gameResult = 1.0;
                break;
            }
        } else if (whiteScore <= -1000) {
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

        // Draw Adjudication: 8 consecutive plies of |score| <= 15 cp (gamePly >= 80)
        if (ply >= 80 && std::abs(bestScore) <= 15) {
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

void run(const Config& config) {
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

    int numThreads = std::max(1, config.threads);
    int targetGames = std::max(1, config.games);
    std::vector<std::thread> workers;

    std::cout << "datagen starting: " << targetGames << " games, "
              << numThreads << " threads, "
              << (config.depth > 0 ? ("depth " + std::to_string(config.depth)) : ("nodes " + std::to_string(config.nodes > 0 ? config.nodes : 5000)))
              << ", output: " << config.outputPath << std::endl;

    auto startTime = std::chrono::steady_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, t]() {
            uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count()
                          + static_cast<uint64_t>(t) * 999983ULL;
            std::mt19937_64 rng(seed);

            auto workerSearch = std::make_unique<Search>();
            workerSearch->set_standalone(true);

            while (true) {
                int gameIdx = gamesStarted.fetch_add(1);
                if (gameIdx >= targetGames) break;

                std::vector<PositionEntry> positions;
                double wdl = 0.5;
                play_game(config, rng, *workerSearch, positions, wdl);

                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    for (const auto& entry : positions) {
                        outFile << entry.fen << " | " << entry.whiteScore << " | "
                                << std::fixed << std::setprecision(1) << wdl << "\n";
                    }
                    outFile.flush();
                    totalPositions += positions.size();
                }

                int done = ++gamesFinished;
                if (done % 10 == 0 || done == targetGames) {
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

    std::cout << "datagen completed: " << targetGames << " games, "
              << totalPositions.load() << " positions saved to "
              << config.outputPath << " in " << elapsedMs << " ms ("
              << (elapsedMs > 0 ? totalPositions.load() * 1000 / elapsedMs : 0) << " pos/s)"
              << std::endl;
}

void run_cli(std::istringstream& is) {
    Config config;
    std::string token;
    while (is >> token) {
        if (token == "games") {
            is >> config.games;
        } else if (token == "threads") {
            is >> config.threads;
        } else if (token == "nodes") {
            is >> config.nodes;
        } else if (token == "depth") {
            is >> config.depth;
        } else if (token == "out" || token == "output") {
            is >> config.outputPath;
        }
    }

    if (!config.outputPath.empty() && (config.outputPath.front() == '"' || config.outputPath.front() == '\'')) {
        config.outputPath = config.outputPath.substr(1);
    }
    if (!config.outputPath.empty() && (config.outputPath.back() == '"' || config.outputPath.back() == '\'')) {
        config.outputPath.pop_back();
    }

    run(config);
}

} // namespace Datagen
