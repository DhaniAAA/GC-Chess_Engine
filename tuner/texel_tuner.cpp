// ============================================================================
// Texel Tuning Implementation - Multi-Threaded Version
// ============================================================================
// Usage: texel_tuner.exe quiet-labeled.epd [max_positions] [iterations]
//
// This tunes evaluation parameters using coordinate descent (local search)
// with multi-threading for faster evaluation.
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

#include "../include/board.hpp"
#include "../include/eval.hpp"
#include "../include/magic.hpp"
#include "../include/zobrist.hpp"
#include "../include/tuning.hpp"

// ============================================================================
// Configuration
// ============================================================================

// Number of threads (auto-detect or set manually)
unsigned int NUM_THREADS = std::thread::hardware_concurrency();

// ============================================================================
// Tunable Parameter Structure
// ============================================================================

struct TunableParam {
    std::string name;
    int* value_ptr;       // Pointer to the value
    int min_val;
    int max_val;
    bool is_mg;           // true = middlegame, false = endgame

    TunableParam(const std::string& n, int* ptr, int min_v, int max_v, bool mg = true)
        : name(n), value_ptr(ptr), min_val(min_v), max_val(max_v), is_mg(mg) {}
};

// ============================================================================
// Training Position Structure
// ============================================================================

struct TrainingPosition {
    std::string fen;
    double result;  // 1.0 = white wins, 0.5 = draw, 0.0 = black wins
};

// ============================================================================
// Global Variables
// ============================================================================

std::vector<TunableParam> params;
std::vector<TrainingPosition> positions;
double K = 1.13;  // Sigmoid scaling constant

// ============================================================================
// Initialize Tunable Parameters (31 parameters total)
// ============================================================================

void init_params() {
    params.clear();

    // ========================================================================
    // Material Values (10 parameters: 5 pieces × 2 phases)
    // ========================================================================
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

    // ========================================================================
    // Piece Activity Bonuses (10 parameters: 5 bonuses × 2 phases)
    // ========================================================================
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

    // ========================================================================
    // Pawn Structure (10 parameters: 5 terms × 2 phases)
    // ========================================================================
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

    // ========================================================================
    // King Safety (1 parameter)
    // ========================================================================
    params.push_back(TunableParam("KingSafetyWeight",       &Tuning::KingSafetyWeight,        30,  150, true));

    std::cout << "Initialized " << params.size() << " tunable parameters\n";
}

// ============================================================================
// Parse Result String to Numeric Value
// ============================================================================

double parse_result(const std::string& result) {
    if (result == "\"1-0\"" || result == "1-0") return 1.0;
    if (result == "\"0-1\"" || result == "0-1") return 0.0;
    if (result == "\"1/2-1/2\"" || result == "1/2-1/2") return 0.5;
    return 0.5;
}

// ============================================================================
// Load Training Positions from EPD File
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
// Sigmoid Function
// ============================================================================

inline double sigmoid(double score, double k) {
    return 1.0 / (1.0 + std::pow(10.0, -k * score / 400.0));
}

// ============================================================================
// Multi-Threaded Error Calculation
// ============================================================================

// Worker function for parallel error calculation
void worker_calculate_error(size_t start, size_t end, double k, double& partial_error) {
    double local_error = 0.0;

    for (size_t i = start; i < end; ++i) {
        // Create a fresh Board for each position (thread-safe)
        Board board(positions[i].fen);

        // Use evaluate_no_cache to avoid race conditions on pawnTable
        int score = Eval::evaluate_no_cache(board);

        double predicted = sigmoid(score, k);
        double error = positions[i].result - predicted;
        local_error += error * error;
    }

    partial_error = local_error;
}

// Calculate error using multiple threads
double calculate_error_mt(double k) {
    std::vector<std::thread> threads;
    std::vector<double> partial_errors(NUM_THREADS, 0.0);

    size_t chunk_size = positions.size() / NUM_THREADS;

    for (unsigned int t = 0; t < NUM_THREADS; ++t) {
        size_t start = t * chunk_size;
        size_t end = (t == NUM_THREADS - 1) ? positions.size() : (t + 1) * chunk_size;

        threads.emplace_back(worker_calculate_error, start, end, k, std::ref(partial_errors[t]));
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Sum all partial errors
    double total_error = 0.0;
    for (double err : partial_errors) {
        total_error += err;
    }

    return total_error / positions.size();
}

// Alias for compatibility
double calculate_error(double k) {
    return calculate_error_mt(k);
}

// ============================================================================
// Find Optimal K Value
// ============================================================================

double find_optimal_k() {
    std::cout << "Finding optimal K value (using " << NUM_THREADS << " threads)...\n";

    double best_k = 1.0;
    double best_error = calculate_error(best_k);

    // Coarse search
    for (double k = 0.3; k <= 2.0; k += 0.1) {
        double error = calculate_error(k);
        std::cout << "  K = " << std::fixed << std::setprecision(2) << k
                  << ", error = " << std::setprecision(6) << error << "\r" << std::flush;
        if (error < best_error) {
            best_error = error;
            best_k = k;
        }
    }

    // Fine search
    for (double k = best_k - 0.1; k <= best_k + 0.1; k += 0.01) {
        double error = calculate_error(k);
        if (error < best_error) {
            best_error = error;
            best_k = k;
        }
    }

    std::cout << "\nOptimal K = " << std::fixed << std::setprecision(4) << best_k
              << " (error = " << best_error << ")\n";

    return best_k;
}

// ============================================================================
// Local Search (Coordinate Descent) - Multi-Threaded
// ============================================================================

void tune_parameters(int iterations = 100) {
    std::cout << "\n=== Starting Texel Tuning (Multi-Threaded Local Search) ===\n";
    std::cout << "Threads: " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << iterations << "\n";
    std::cout << "Positions: " << positions.size() << "\n";
    std::cout << "Parameters: " << params.size() << "\n\n";

    double best_error = calculate_error(K);
    std::cout << "Initial error: " << std::fixed << std::setprecision(8) << best_error << "\n\n";
    double initial_error = best_error;

    int step = 5;  // Initial step size
    int no_improvement_count = 0;

    for (int iter = 1; iter <= iterations; iter++) {
        auto start = std::chrono::steady_clock::now();

        bool improved_this_iter = false;
        int params_changed = 0;

        // Try each parameter
        for (auto& p : params) {
            int original = *p.value_ptr;

            // Try increasing
            int new_val_up = std::clamp(original + step, p.min_val, p.max_val);
            if (new_val_up != original) {
                *p.value_ptr = new_val_up;
                double error_up = calculate_error(K);

                if (error_up < best_error) {
                    best_error = error_up;
                    improved_this_iter = true;
                    params_changed++;
                    continue;  // Keep the change
                }
            }

            // Try decreasing
            int new_val_down = std::clamp(original - step, p.min_val, p.max_val);
            if (new_val_down != original) {
                *p.value_ptr = new_val_down;
                double error_down = calculate_error(K);

                if (error_down < best_error) {
                    best_error = error_down;
                    improved_this_iter = true;
                    params_changed++;
                    continue;  // Keep the change
                }
            }

            // Neither direction improved, restore original
            *p.value_ptr = original;
        }

        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();

        double improvement = (initial_error - best_error) * 100 / initial_error;

        std::cout << "Iter " << std::setw(3) << iter
                  << " | Error: " << std::setprecision(8) << best_error
                  << " | Imp: " << std::setprecision(4) << improvement << "%"
                  << " | Changed: " << params_changed
                  << " | Step: " << step
                  << " | Time: " << std::setprecision(1) << elapsed << "s\n";

        // Reduce step size if no improvement
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

        // Print values every 5 iterations
        if (iter % 5 == 0) {
            std::cout << "\n--- Current Values ---\n";
            for (const auto& p : params) {
                std::cout << p.name << " = " << *p.value_ptr << "\n";
            }
            std::cout << "\n";
        }

        // Early stop if step is 1 and no improvement
        if (step == 1 && no_improvement_count >= 3) {
            std::cout << "Converged!\n";
            break;
        }
    }

    // Print final values in EvalScore format
    std::cout << "\n=== FINAL TUNED VALUES ===\n\n";
    std::cout << "// Copy to tuning.cpp:\n\n";

    // Material Values (10 params: 5 pieces × 2 phases)
    std::cout << "EvalScore PawnValue           = S(" << std::setw(4) << Tuning::PawnValue.mg << ", " << std::setw(4) << Tuning::PawnValue.eg << ");\n";
    std::cout << "EvalScore KnightValue         = S(" << std::setw(4) << Tuning::KnightValue.mg << ", " << std::setw(4) << Tuning::KnightValue.eg << ");\n";
    std::cout << "EvalScore BishopValue         = S(" << std::setw(4) << Tuning::BishopValue.mg << ", " << std::setw(4) << Tuning::BishopValue.eg << ");\n";
    std::cout << "EvalScore RookValue           = S(" << std::setw(4) << Tuning::RookValue.mg << ", " << std::setw(4) << Tuning::RookValue.eg << ");\n";
    std::cout << "EvalScore QueenValue          = S(" << std::setw(4) << Tuning::QueenValue.mg << ", " << std::setw(4) << Tuning::QueenValue.eg << ");\n";

    // Piece Activity Bonuses (10 params: 5 bonuses × 2 phases)
    std::cout << "EvalScore BishopPairBonus     = S(" << std::setw(4) << Tuning::BishopPairBonus.mg << ", " << std::setw(4) << Tuning::BishopPairBonus.eg << ");\n";
    std::cout << "EvalScore RookOpenFileBonus   = S(" << std::setw(4) << Tuning::RookOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookOpenFileBonus.eg << ");\n";
    std::cout << "EvalScore RookSemiOpenFileBonus = S(" << std::setw(4) << Tuning::RookSemiOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookSemiOpenFileBonus.eg << ");\n";
    std::cout << "EvalScore RookOnSeventhBonus  = S(" << std::setw(4) << Tuning::RookOnSeventhBonus.mg << ", " << std::setw(4) << Tuning::RookOnSeventhBonus.eg << ");\n";
    std::cout << "EvalScore KnightOutpostBonus  = S(" << std::setw(4) << Tuning::KnightOutpostBonus.mg << ", " << std::setw(4) << Tuning::KnightOutpostBonus.eg << ");\n";

    // Pawn Structure (10 params: 5 terms × 2 phases)
    std::cout << "EvalScore IsolatedPawnPenalty = S(" << std::setw(4) << Tuning::IsolatedPawnPenalty.mg << ", " << std::setw(4) << Tuning::IsolatedPawnPenalty.eg << ");\n";
    std::cout << "EvalScore DoubledPawnPenalty  = S(" << std::setw(4) << Tuning::DoubledPawnPenalty.mg << ", " << std::setw(4) << Tuning::DoubledPawnPenalty.eg << ");\n";
    std::cout << "EvalScore BackwardPawnPenalty = S(" << std::setw(4) << Tuning::BackwardPawnPenalty.mg << ", " << std::setw(4) << Tuning::BackwardPawnPenalty.eg << ");\n";
    std::cout << "EvalScore ConnectedPawnBonus  = S(" << std::setw(4) << Tuning::ConnectedPawnBonus.mg << ", " << std::setw(4) << Tuning::ConnectedPawnBonus.eg << ");\n";
    std::cout << "EvalScore PhalanxBonus        = S(" << std::setw(4) << Tuning::PhalanxBonus.mg << ", " << std::setw(4) << Tuning::PhalanxBonus.eg << ");\n";

    // King Safety (1 param)
    std::cout << "int KingSafetyWeight          = " << std::setw(4) << Tuning::KingSafetyWeight << ";\n";

    double improvement = (initial_error - best_error) * 100 / initial_error;
    std::cout << "\n=== Tuning Complete ===\n";
    std::cout << "Total Parameters: 31 (15 EvalScore x 2 + 1 int)\n";
    std::cout << "Initial error: " << std::setprecision(8) << initial_error << "\n";
    std::cout << "Final error:   " << best_error << "\n";
    std::cout << "Improvement:   " << std::setprecision(4) << improvement << "%\n";
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "=================================\n";
    std::cout << "  GC-Engine Texel Tuner v3\n";
    std::cout << "   (Multi-Threaded Version)\n";
    std::cout << "=================================\n\n";

    // Ensure at least 1 thread
    if (NUM_THREADS == 0) NUM_THREADS = 1;
    std::cout << "Using " << NUM_THREADS << " threads\n\n";

    // Initialize engine components
    Magics::init();
    Zobrist::init();

    // Determine input file
    std::string epd_file = "tuner/quiet-labeled.epd";
    if (argc > 1) epd_file = argv[1];

    // Load positions
    size_t max_pos = 500000;  // Default 500K for reasonable speed with multi-threading
    if (argc > 2) max_pos = std::stoull(argv[2]);

    if (!load_positions(epd_file, max_pos)) {
        std::cerr << "Failed to load positions!\n";
        return 1;
    }

    // Initialize parameters
    init_params();

    // Find optimal K
    K = find_optimal_k();

    // Run tuning
    int iterations = 100;
    if (argc > 3) iterations = std::stoi(argv[3]);

    tune_parameters(iterations);

    return 0;
}
