// ============================================================================
// Texel Tuning Implementation - Ultra-Fast Version (v6)
// ============================================================================
// Usage: tuner.exe <epd_file> [max_positions] [iterations] [manual_K]
//
// OPTIMIZED APPROACH:
// 1. Pre-compute base scores for all positions ONCE
// 2. When testing a parameter change, only recalculate error (not re-evaluate)
// 3. Use multi-threading to test multiple parameters simultaneously
// 4. K value minimum clamped to 0.5 to prevent flat sigmoid
// ============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>

#include "../include/board.hpp"
#include "../include/eval.hpp"
#include "../include/magic.hpp"
#include "../include/zobrist.hpp"
#include "../include/tuning.hpp"

// ============================================================================
// Configuration
// ============================================================================

unsigned int NUM_THREADS = std::thread::hardware_concurrency();

// ============================================================================
// Tunable Parameter Structure
// ============================================================================

struct TunableParam {
    std::string name;
    int* value_ptr;
    int min_val;
    int max_val;
    bool is_mg;

    TunableParam(const std::string& n, int* ptr, int min_v, int max_v, bool mg = true)
        : name(n), value_ptr(ptr), min_val(min_v), max_val(max_v), is_mg(mg) {}
};

// ============================================================================
// Training Position with Pre-computed Data
// ============================================================================

struct TrainingPosition {
    std::string fen;
    double result;
    int base_score;  // Pre-computed evaluation score
};

// ============================================================================
// Global Variables
// ============================================================================

std::vector<TunableParam> params;
std::vector<TrainingPosition> positions;
double K = 1.13;

// ============================================================================
// Initialize Tunable Parameters
// ============================================================================

void init_params() {
    params.clear();

    // Material Values
    params.push_back(TunableParam("PawnValue_MG",           &Tuning::PawnValue.mg,            60,  150, true));
    params.push_back(TunableParam("PawnValue_EG",           &Tuning::PawnValue.eg,            80,  180, false));
    params.push_back(TunableParam("KnightValue_MG",         &Tuning::KnightValue.mg,         200,  400, true));
    params.push_back(TunableParam("KnightValue_EG",         &Tuning::KnightValue.eg,         200,  380, false));
    params.push_back(TunableParam("BishopValue_MG",         &Tuning::BishopValue.mg,         200,  400, true));
    params.push_back(TunableParam("BishopValue_EG",         &Tuning::BishopValue.eg,         200,  400, false));
    params.push_back(TunableParam("RookValue_MG",           &Tuning::RookValue.mg,           350,  600, true));
    params.push_back(TunableParam("RookValue_EG",           &Tuning::RookValue.eg,           400,  700, false));
    params.push_back(TunableParam("QueenValue_MG",          &Tuning::QueenValue.mg,          700, 1200, true));
    params.push_back(TunableParam("QueenValue_EG",          &Tuning::QueenValue.eg,          800, 1300, false));

    // Piece Activity Bonuses
    params.push_back(TunableParam("BishopPairBonus_MG",     &Tuning::BishopPairBonus.mg,       0,  100, true));
    params.push_back(TunableParam("BishopPairBonus_EG",     &Tuning::BishopPairBonus.eg,       0,  120, false));
    params.push_back(TunableParam("RookOpenFileBonus_MG",   &Tuning::RookOpenFileBonus.mg,     0,   60, true));
    params.push_back(TunableParam("RookOpenFileBonus_EG",   &Tuning::RookOpenFileBonus.eg,     0,   50, false));
    params.push_back(TunableParam("RookSemiOpenFileBonus_MG", &Tuning::RookSemiOpenFileBonus.mg, 0,   50, true));
    params.push_back(TunableParam("RookSemiOpenFileBonus_EG", &Tuning::RookSemiOpenFileBonus.eg, 0,   40, false));
    params.push_back(TunableParam("RookOnSeventhBonus_MG",  &Tuning::RookOnSeventhBonus.mg,    0,   60, true));
    params.push_back(TunableParam("RookOnSeventhBonus_EG",  &Tuning::RookOnSeventhBonus.eg,    0,   80, false));
    params.push_back(TunableParam("KnightOutpostBonus_MG",  &Tuning::KnightOutpostBonus.mg,    0,   60, true));
    params.push_back(TunableParam("KnightOutpostBonus_EG",  &Tuning::KnightOutpostBonus.eg,    0,   50, false));

    // Pawn Structure
    params.push_back(TunableParam("IsolatedPawnPenalty_MG", &Tuning::IsolatedPawnPenalty.mg, -60,    0, true));
    params.push_back(TunableParam("IsolatedPawnPenalty_EG", &Tuning::IsolatedPawnPenalty.eg, -60,    0, false));
    params.push_back(TunableParam("DoubledPawnPenalty_MG",  &Tuning::DoubledPawnPenalty.mg,  -60,    0, true));
    params.push_back(TunableParam("DoubledPawnPenalty_EG",  &Tuning::DoubledPawnPenalty.eg,  -60,    0, false));
    params.push_back(TunableParam("BackwardPawnPenalty_MG", &Tuning::BackwardPawnPenalty.mg, -50,    0, true));
    params.push_back(TunableParam("BackwardPawnPenalty_EG", &Tuning::BackwardPawnPenalty.eg, -50,    0, false));
    params.push_back(TunableParam("ConnectedPawnBonus_MG",  &Tuning::ConnectedPawnBonus.mg,    0,   50, true));
    params.push_back(TunableParam("ConnectedPawnBonus_EG",  &Tuning::ConnectedPawnBonus.eg,    0,   50, false));
    params.push_back(TunableParam("PhalanxBonus_MG",        &Tuning::PhalanxBonus.mg,          0,   50, true));
    params.push_back(TunableParam("PhalanxBonus_EG",        &Tuning::PhalanxBonus.eg,          0,   50, false));

    // King Safety
    params.push_back(TunableParam("KingSafetyWeight",       &Tuning::KingSafetyWeight,        30,  150, true));

    std::cout << "Initialized " << params.size() << " tunable parameters\n";
}

// ============================================================================
// Parse Result String
// ============================================================================

double parse_result(const std::string& result) {
    if (result == "\"1-0\"" || result == "1-0") return 1.0;
    if (result == "\"0-1\"" || result == "0-1") return 0.0;
    if (result == "\"1/2-1/2\"" || result == "1/2-1/2") return 0.5;
    return 0.5;
}

// ============================================================================
// Load and Pre-evaluate Positions
// ============================================================================

bool load_positions(const std::string& filename, size_t max_positions = 0) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return false;
    }

    positions.clear();
    std::string line;
    size_t count = 0;

    std::cout << "Loading positions from " << filename << "...\n";

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        size_t quote1 = line.find('"');
        size_t quote2 = line.rfind('"');
        if (quote1 == std::string::npos || quote2 == quote1) continue;

        std::string result = line.substr(quote1 + 1, quote2 - quote1 - 1);

        size_t c9_pos = line.find(" c9 ");
        if (c9_pos == std::string::npos) continue;

        TrainingPosition pos;
        pos.fen = line.substr(0, c9_pos);
        pos.result = parse_result(result);
        pos.base_score = 0;  // Will be computed later
        positions.push_back(pos);

        count++;
        if (count % 100000 == 0) {
            std::cout << "  Loaded " << count << " positions...\r" << std::flush;
        }

        if (max_positions > 0 && count >= max_positions) break;
    }

    std::cout << "\nLoaded " << positions.size() << " positions\n";
    return positions.size() > 0;
}

// ============================================================================
// Pre-evaluate all positions (multi-threaded)
// ============================================================================

void precompute_scores_worker(size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
        Board board(positions[i].fen);
        positions[i].base_score = Eval::evaluate_no_cache(board);
    }
}

void precompute_all_scores() {
    std::cout << "Pre-computing scores for all positions...\n";
    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    size_t chunk_size = positions.size() / NUM_THREADS;

    for (unsigned int t = 0; t < NUM_THREADS; ++t) {
        size_t s = t * chunk_size;
        size_t e = (t == NUM_THREADS - 1) ? positions.size() : (t + 1) * chunk_size;
        threads.emplace_back(precompute_scores_worker, s, e);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    std::cout << "  Done in " << std::fixed << std::setprecision(1) << elapsed << "s\n";
}

// ============================================================================
// Sigmoid Function
// ============================================================================

inline double sigmoid(double score, double k) {
    return 1.0 / (1.0 + std::pow(10.0, -k * score / 400.0));
}

// ============================================================================
// Calculate Error from Pre-computed Scores (multi-threaded)
// ============================================================================

void calc_error_worker(size_t start, size_t end, double k, double& partial_error) {
    double local_error = 0.0;
    for (size_t i = start; i < end; ++i) {
        double predicted = sigmoid(positions[i].base_score, k);
        double error = positions[i].result - predicted;
        local_error += error * error;
    }
    partial_error = local_error;
}

double calculate_error_fast(double k) {
    std::vector<std::thread> threads;
    std::vector<double> partial_errors(NUM_THREADS, 0.0);
    size_t chunk_size = positions.size() / NUM_THREADS;

    for (unsigned int t = 0; t < NUM_THREADS; ++t) {
        size_t s = t * chunk_size;
        size_t e = (t == NUM_THREADS - 1) ? positions.size() : (t + 1) * chunk_size;
        threads.emplace_back(calc_error_worker, s, e, k, std::ref(partial_errors[t]));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    double total = 0.0;
    for (double err : partial_errors) total += err;
    return total / positions.size();
}

// ============================================================================
// Re-evaluate all positions with current parameters (multi-threaded)
// ============================================================================

void reevaluate_all_scores() {
    std::vector<std::thread> threads;
    size_t chunk_size = positions.size() / NUM_THREADS;

    for (unsigned int t = 0; t < NUM_THREADS; ++t) {
        size_t s = t * chunk_size;
        size_t e = (t == NUM_THREADS - 1) ? positions.size() : (t + 1) * chunk_size;
        threads.emplace_back(precompute_scores_worker, s, e);
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

// ============================================================================
// Find Optimal K
// ============================================================================

double find_optimal_k(double manual_k = 0.0) {
    // If manual K is provided, use it
    if (manual_k > 0.0) {
        std::cout << "Using manual K = " << std::fixed << std::setprecision(4) << manual_k << "\n";
        return manual_k;
    }

    std::cout << "Finding optimal K value...\n";

    double best_k = 1.13;  // Start with typical value
    double best_error = calculate_error_fast(best_k);

    // Coarse search - start from 0.5 (K < 0.5 is usually too flat)
    for (double k = 0.5; k <= 2.5; k += 0.1) {
        double error = calculate_error_fast(k);
        std::cout << "  K = " << std::fixed << std::setprecision(2) << k
                  << ", error = " << std::setprecision(6) << error << "\n";
        if (error < best_error) {
            best_error = error;
            best_k = k;
        }
    }

    // Fine search around best K
    double search_start = std::max(0.5, best_k - 0.1);
    double search_end = best_k + 0.1;
    for (double k = search_start; k <= search_end; k += 0.01) {
        double error = calculate_error_fast(k);
        if (error < best_error) {
            best_error = error;
            best_k = k;
        }
    }

    // Sanity check: K should be between 0.5 and 2.5
    if (best_k < 0.5) {
        std::cout << "Warning: Optimal K (" << best_k << ") is too low, clamping to 0.5\n";
        best_k = 0.5;
    }

    std::cout << "Optimal K = " << std::fixed << std::setprecision(4) << best_k
              << " (error = " << best_error << ")\n";

    return best_k;
}

// ============================================================================
// Tune Parameters using Local Search with Parallel Evaluation
// ============================================================================

void tune_parameters(int iterations = 100) {
    std::cout << "\n=== Starting Texel Tuning (Fast Local Search) ===\n";
    std::cout << "Threads: " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << iterations << "\n";
    std::cout << "Positions: " << positions.size() << "\n";
    std::cout << "Parameters: " << params.size() << "\n\n";

    double best_error = calculate_error_fast(K);
    std::cout << "Initial error: " << std::fixed << std::setprecision(8) << best_error << "\n\n";
    double initial_error = best_error;

    int step = 5;
    int no_improvement_count = 0;

    for (int iter = 1; iter <= iterations; iter++) {
        auto start_time = std::chrono::steady_clock::now();

        bool improved_this_iter = false;
        int params_changed = 0;

        // Try each parameter
        for (size_t p = 0; p < params.size(); ++p) {
            int original = *params[p].value_ptr;

            // Try increasing
            int new_val_up = std::clamp(original + step, params[p].min_val, params[p].max_val);
            if (new_val_up != original) {
                *params[p].value_ptr = new_val_up;
                reevaluate_all_scores();  // Re-evaluate with new param
                double error_up = calculate_error_fast(K);

                if (error_up < best_error) {
                    best_error = error_up;
                    improved_this_iter = true;
                    params_changed++;
                    continue;
                }
            }

            // Try decreasing
            int new_val_down = std::clamp(original - step, params[p].min_val, params[p].max_val);
            if (new_val_down != original) {
                *params[p].value_ptr = new_val_down;
                reevaluate_all_scores();
                double error_down = calculate_error_fast(K);

                if (error_down < best_error) {
                    best_error = error_down;
                    improved_this_iter = true;
                    params_changed++;
                    continue;
                }
            }

            // Restore and re-evaluate
            *params[p].value_ptr = original;
            reevaluate_all_scores();
        }

        auto end_time = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end_time - start_time).count();

        double improvement = (initial_error - best_error) * 100 / initial_error;

        std::cout << "Iter " << std::setw(3) << iter
                  << " | Error: " << std::setprecision(8) << best_error
                  << " | Imp: " << std::setprecision(4) << improvement << "%"
                  << " | Changed: " << params_changed
                  << " | Step: " << step
                  << " | Time: " << std::setprecision(1) << elapsed << "s\n";

        if (!improved_this_iter) {
            no_improvement_count++;
            if (no_improvement_count >= 2 && step > 1) {
                step = std::max(1, step / 2);
                no_improvement_count = 0;
                std::cout << "  Reducing step size to " << step << "\n";
            }
        } else {
            no_improvement_count = 0;
        }

        if (iter % 5 == 0) {
            std::cout << "\n--- Current Values ---\n";
            for (const auto& param : params) {
                std::cout << param.name << " = " << *param.value_ptr << "\n";
            }
            std::cout << "\n";
        }

        if (step == 1 && no_improvement_count >= 5) {
            std::cout << "Converged after " << iter << " iterations!\n";
            break;
        }
    }

    // Print final values
    std::cout << "\n=== FINAL TUNED VALUES ===\n\n";
    std::cout << "// Copy to tuning.cpp:\n\n";

    std::cout << "EvalScore PawnValue           = S(" << std::setw(4) << Tuning::PawnValue.mg << ", " << std::setw(4) << Tuning::PawnValue.eg << ");\n";
    std::cout << "EvalScore KnightValue         = S(" << std::setw(4) << Tuning::KnightValue.mg << ", " << std::setw(4) << Tuning::KnightValue.eg << ");\n";
    std::cout << "EvalScore BishopValue         = S(" << std::setw(4) << Tuning::BishopValue.mg << ", " << std::setw(4) << Tuning::BishopValue.eg << ");\n";
    std::cout << "EvalScore RookValue           = S(" << std::setw(4) << Tuning::RookValue.mg << ", " << std::setw(4) << Tuning::RookValue.eg << ");\n";
    std::cout << "EvalScore QueenValue          = S(" << std::setw(4) << Tuning::QueenValue.mg << ", " << std::setw(4) << Tuning::QueenValue.eg << ");\n";

    std::cout << "EvalScore BishopPairBonus     = S(" << std::setw(4) << Tuning::BishopPairBonus.mg << ", " << std::setw(4) << Tuning::BishopPairBonus.eg << ");\n";
    std::cout << "EvalScore RookOpenFileBonus   = S(" << std::setw(4) << Tuning::RookOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookOpenFileBonus.eg << ");\n";
    std::cout << "EvalScore RookSemiOpenFileBonus = S(" << std::setw(4) << Tuning::RookSemiOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookSemiOpenFileBonus.eg << ");\n";
    std::cout << "EvalScore RookOnSeventhBonus  = S(" << std::setw(4) << Tuning::RookOnSeventhBonus.mg << ", " << std::setw(4) << Tuning::RookOnSeventhBonus.eg << ");\n";
    std::cout << "EvalScore KnightOutpostBonus  = S(" << std::setw(4) << Tuning::KnightOutpostBonus.mg << ", " << std::setw(4) << Tuning::KnightOutpostBonus.eg << ");\n";

    std::cout << "EvalScore IsolatedPawnPenalty = S(" << std::setw(4) << Tuning::IsolatedPawnPenalty.mg << ", " << std::setw(4) << Tuning::IsolatedPawnPenalty.eg << ");\n";
    std::cout << "EvalScore DoubledPawnPenalty  = S(" << std::setw(4) << Tuning::DoubledPawnPenalty.mg << ", " << std::setw(4) << Tuning::DoubledPawnPenalty.eg << ");\n";
    std::cout << "EvalScore BackwardPawnPenalty = S(" << std::setw(4) << Tuning::BackwardPawnPenalty.mg << ", " << std::setw(4) << Tuning::BackwardPawnPenalty.eg << ");\n";
    std::cout << "EvalScore ConnectedPawnBonus  = S(" << std::setw(4) << Tuning::ConnectedPawnBonus.mg << ", " << std::setw(4) << Tuning::ConnectedPawnBonus.eg << ");\n";
    std::cout << "EvalScore PhalanxBonus        = S(" << std::setw(4) << Tuning::PhalanxBonus.mg << ", " << std::setw(4) << Tuning::PhalanxBonus.eg << ");\n";

    std::cout << "int KingSafetyWeight          = " << std::setw(4) << Tuning::KingSafetyWeight << ";\n";

    double improvement = (initial_error - best_error) * 100 / initial_error;
    std::cout << "\n=== Tuning Complete ===\n";
    std::cout << "Initial error: " << std::setprecision(8) << initial_error << "\n";
    std::cout << "Final error:   " << best_error << "\n";
    std::cout << "Improvement:   " << std::setprecision(4) << improvement << "%\n";
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=================================\n";
    std::cout << "  GC-Engine Texel Tuner v6\n";
    std::cout << "  (Ultra-Fast Version)\n";
    std::cout << "=================================\n\n";
    std::cout << "Usage: tuner.exe <epd_file> [max_positions] [iterations] [manual_K]\n";
    std::cout << "  epd_file      : Path to labeled EPD file\n";
    std::cout << "  max_positions : Maximum positions to load (0 = all)\n";
    std::cout << "  iterations    : Number of tuning iterations (default: 100)\n";
    std::cout << "  manual_K      : Optional manual K value (default: auto-find)\n\n";

    if (NUM_THREADS == 0) NUM_THREADS = 1;
    std::cout << "Using " << NUM_THREADS << " threads\n\n";

    Magics::init();
    Zobrist::init();

    std::string epd_file = "tuner/quiet-labeled.epd";
    if (argc > 1) epd_file = argv[1];

    size_t max_pos = 500000;
    if (argc > 2) max_pos = std::stoull(argv[2]);

    if (!load_positions(epd_file, max_pos)) {
        std::cerr << "Failed to load positions!\n";
        return 1;
    }

    init_params();

    // Pre-compute initial scores
    precompute_all_scores();

    // Find optimal K (or use manual K if provided)
    double manual_k = 0.0;
    if (argc > 4) manual_k = std::stod(argv[4]);
    K = find_optimal_k(manual_k);

    // Run tuning
    int iterations = 100;
    if (argc > 3) iterations = std::stoi(argv[3]);

    tune_parameters(iterations);

    return 0;
}
