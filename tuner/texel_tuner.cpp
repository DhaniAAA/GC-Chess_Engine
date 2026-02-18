// ============================================================================
// Texel Tuning Implementation - Ultra-Fast Version (v7)
// ============================================================================
// Usage: tuner.exe <epd_file> [max_positions] [iterations] [manual_K]
//
// OPTIMIZED APPROACH:
// 1. Pre-compute base scores for all positions ONCE
// 2. When testing a parameter change, only recalculate error (not re-evaluate)
// 3. Use multi-threading to test multiple parameters simultaneously
// 4. K value minimum clamped to 0.5 to prevent flat sigmoid
//
// v7: Expanded to ~200 tunable parameters (mobility, passed pawns,
//     king safety, pawn structure, piece activity, material imbalance)
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
// Monotonic Group (for arrays where values must be non-decreasing)
// ============================================================================

struct MonotonicGroup {
    std::string name;
    EvalScore* array;   // pointer to the first element
    int size;           // number of elements
    int start_idx;      // first tunable index (skip [0] if always 0)
    int end_idx;        // last tunable index (skip [7] if always 0)
};

// ============================================================================
// Global Variables
// ============================================================================

std::vector<TunableParam> params;
std::vector<TrainingPosition> positions;
std::vector<MonotonicGroup> monotonic_groups;
double K = 1.13;

// ============================================================================
// Check if all monotonic constraints are satisfied
// Returns true if all groups are non-decreasing (MG and EG separately)
// ============================================================================

bool check_monotonicity() {
    for (const auto& group : monotonic_groups) {
        for (int i = group.start_idx + 1; i <= group.end_idx; i++) {
            // MG must be non-decreasing
            if (group.array[i].mg < group.array[i - 1].mg) return false;
            // EG must be non-decreasing
            if (group.array[i].eg < group.array[i - 1].eg) return false;
        }
    }
    return true;
}

// ============================================================================
// Helper: add EvalScore param (adds both MG and EG)
// ============================================================================

void add_eval_param(const std::string& name, EvalScore& es, int min_mg, int max_mg, int min_eg, int max_eg) {
    params.push_back(TunableParam(name + "_MG", &es.mg, min_mg, max_mg, true));
    params.push_back(TunableParam(name + "_EG", &es.eg, min_eg, max_eg, false));
}

// ============================================================================
// Initialize Tunable Parameters
// ============================================================================

void init_params() {
    params.clear();

    // ====================================================================
    // Material Values (10 params)
    // ====================================================================
    add_eval_param("PawnValue",     Tuning::PawnValue,       70, 130,   80, 160);
    add_eval_param("KnightValue",   Tuning::KnightValue,    280, 380,  290, 390);
    add_eval_param("BishopValue",   Tuning::BishopValue,    290, 390,  300, 400);
    add_eval_param("RookValue",     Tuning::RookValue,      450, 560,  470, 600);
    add_eval_param("QueenValue",    Tuning::QueenValue,     880, 1080, 900, 1100);

    // ====================================================================
    // Piece Activity Bonuses (10 params)
    // ====================================================================
    add_eval_param("BishopPairBonus",       Tuning::BishopPairBonus,        0,  60,    0,  80);
    add_eval_param("RookOpenFileBonus",     Tuning::RookOpenFileBonus,      5,  60,    0,  50);
    add_eval_param("RookSemiOpenFileBonus", Tuning::RookSemiOpenFileBonus,  0,  40,    0,  30);
    add_eval_param("RookOnSeventhBonus",    Tuning::RookOnSeventhBonus,     0,  50,    0,  60);
    add_eval_param("KnightOutpostBonus",    Tuning::KnightOutpostBonus,     5,  60,    0,  40);

    // ====================================================================
    // Pawn Structure (18 params)
    // ====================================================================
    add_eval_param("IsolatedPawnPenalty",   Tuning::IsolatedPawnPenalty, -40,   0, -40,   0);
    add_eval_param("DoubledPawnPenalty",    Tuning::DoubledPawnPenalty,  -40,   0, -40,   0);
    add_eval_param("BackwardPawnPenalty",   Tuning::BackwardPawnPenalty, -30,   0, -30,   0);
    add_eval_param("ConnectedPawnBonus",    Tuning::ConnectedPawnBonus,    0,  30,    0,  30);
    add_eval_param("PhalanxBonus",          Tuning::PhalanxBonus,          0,  30,    0,  40);
    add_eval_param("PawnIslandPenalty",     Tuning::PawnIslandPenalty,   -20,   0, -20,   0);
    add_eval_param("PawnChainBonus",        Tuning::PawnChainBonus,        0,  15,    0,  10);
    add_eval_param("PawnChainBaseBonus",    Tuning::PawnChainBaseBonus,    0,  20,    0,  15);
    add_eval_param("CentralPawnBonus",      Tuning::CentralPawnBonus,      0,  30,    0,  15);
    add_eval_param("PawnDuoBonus",          Tuning::PawnDuoBonus,          0,  15,    0,  20);
    add_eval_param("OutpostHolePenalty",    Tuning::OutpostHolePenalty,  -30,   0, -20,   0);
    add_eval_param("HangingPawnPenalty",    Tuning::HangingPawnPenalty,  -25,   0, -25,   0);
    add_eval_param("BackwardOnHalfOpen",    Tuning::BackwardOnHalfOpen, -25,   0, -15,   0);
    add_eval_param("CentralLeverBonus",     Tuning::CentralLeverBonus,    0,  20,    0,  10);

    // ====================================================================
    // Passed Pawn Bonuses - ranks 1-6 (rank 0 and 7 are always 0)
    // (24 params: 4 arrays × 6 ranks × MG/EG, but we skip [0] and [7])
    // ====================================================================
    for (int r = 1; r <= 6; r++) {
        std::string rstr = std::to_string(r);
        add_eval_param("PassedPawnR" + rstr,       Tuning::PassedPawnBonus[r],        0, 200,    0, 300);
        add_eval_param("ConnPassedR" + rstr,       Tuning::ConnectedPassedBonus[r],   0, 120,    0, 180);
        add_eval_param("ProtPassedR" + rstr,       Tuning::ProtectedPassedBonus[r],   0, 200,    0, 300);
        add_eval_param("CandPassedR" + rstr,       Tuning::CandidatePassedBonus[r],   0, 100,    0, 150);
    }
    params.push_back(TunableParam("RuleOfSquareBonus", &Tuning::RuleOfSquareBonus, 50, 300, true));

    // ====================================================================
    // Mobility Tables (with monotonicity constraints)
    // Knight: 9 entries (18 params) - values MUST be non-decreasing
    // ====================================================================
    for (int i = 0; i < 9; i++) {
        std::string istr = std::to_string(i);
        add_eval_param("KnMob" + istr, Tuning::KnightMobility[i], -60, 40, -60, 40);
    }
    monotonic_groups.push_back({"KnightMobility", Tuning::KnightMobility, 9, 0, 8});

    // Bishop: 14 entries (28 params)
    for (int i = 0; i < 14; i++) {
        std::string istr = std::to_string(i);
        add_eval_param("BiMob" + istr, Tuning::BishopMobility[i], -50, 50, -50, 50);
    }
    monotonic_groups.push_back({"BishopMobility", Tuning::BishopMobility, 14, 0, 13});

    // Rook: 15 entries (30 params)
    for (int i = 0; i < 15; i++) {
        std::string istr = std::to_string(i);
        add_eval_param("RkMob" + istr, Tuning::RookMobility[i], -40, 40, -50, 50);
    }
    monotonic_groups.push_back({"RookMobility", Tuning::RookMobility, 15, 0, 14});

    // Queen: 28 entries (56 params)
    for (int i = 0; i < 28; i++) {
        std::string istr = std::to_string(i);
        add_eval_param("QnMob" + istr, Tuning::QueenMobility[i], -40, 30, -40, 30);
    }
    monotonic_groups.push_back({"QueenMobility", Tuning::QueenMobility, 28, 0, 27});

    // Passed pawn tables: rank bonuses should also be non-decreasing (rank 1-6)
    // (rank 0 and 7 are always 0, so we constrain indices 1 through 6)
    monotonic_groups.push_back({"PassedPawnBonus",       Tuning::PassedPawnBonus,       8, 1, 6});
    monotonic_groups.push_back({"ConnectedPassedBonus",  Tuning::ConnectedPassedBonus,  8, 1, 6});
    monotonic_groups.push_back({"ProtectedPassedBonus",  Tuning::ProtectedPassedBonus,  8, 1, 6});
    monotonic_groups.push_back({"CandidatePassedBonus",  Tuning::CandidatePassedBonus,  8, 1, 6});

    // ====================================================================
    // King Safety (11 params)
    // ====================================================================
    params.push_back(TunableParam("KingSafetyWeight",      &Tuning::KingSafetyWeight,       30, 200, true));
    params.push_back(TunableParam("KnightAttackWeight",    &Tuning::KnightAttackWeight,      1,   6, true));
    params.push_back(TunableParam("BishopAttackWeight",    &Tuning::BishopAttackWeight,      1,   6, true));
    params.push_back(TunableParam("RookAttackWeight",      &Tuning::RookAttackWeight,        1,   8, true));
    params.push_back(TunableParam("QueenAttackWeight",     &Tuning::QueenAttackWeight,       2,  10, true));
    params.push_back(TunableParam("InnerRingAttackWeight", &Tuning::InnerRingAttackWeight,   1,   6, true));
    params.push_back(TunableParam("OuterRingAttackWeight", &Tuning::OuterRingAttackWeight,   0,   4, true));
    add_eval_param("KingSemiOpenFilePenalty", Tuning::KingSemiOpenFilePenalty, 0, 40, 0, 15);
    add_eval_param("KingOpenFilePenalty",     Tuning::KingOpenFilePenalty,     5, 50, 0, 20);
    for (int i = 1; i <= 3; i++) {
        std::string istr = std::to_string(i);
        params.push_back(TunableParam("PawnShieldBonus" + istr, &Tuning::PawnShieldBonus[i], 0, 50, true));
    }

    // ====================================================================
    // Piece Activity (14 params)
    // ====================================================================
    add_eval_param("KnightOnRim",          Tuning::KnightOnRim,         -25,   0, -20,   0);
    add_eval_param("BishopLongDiagonal",   Tuning::BishopLongDiagonal,    0,  30,    0,  20);
    add_eval_param("RookBehindPasser",     Tuning::RookBehindPasser,      0,  40,    0,  50);
    add_eval_param("TrappedBishopPenalty", Tuning::TrappedBishopPenalty,-150,   0,-120,   0);
    add_eval_param("TrappedRookPenalty",   Tuning::TrappedRookPenalty,  -80,   0, -60,   0);
    add_eval_param("HighMobilityBonus",    Tuning::HighMobilityBonus,     0,  25,    0,  30);
    add_eval_param("LowMobilityPenalty",   Tuning::LowMobilityPenalty,  -30,   0, -40,   0);

    // ====================================================================
    // Material Imbalance (10 params)
    // ====================================================================
    add_eval_param("RookPairBonus",        Tuning::RookPairBonus,         0,  30,    0,  50);
    add_eval_param("BishopKnightCombo",    Tuning::BishopKnightCombo,     0,  25,    0,  15);
    add_eval_param("KnightPairPenalty",    Tuning::KnightPairPenalty,   -25,   0, -25,   0);
    add_eval_param("RooksWithoutQueens",   Tuning::RooksWithoutQueens,    0,  20,    0,  40);
    add_eval_param("QueenWithoutRooks",    Tuning::QueenWithoutRooks,   -20,   0, -30,   0);

    // ====================================================================
    // King Safety Extended
    // ====================================================================
    // SafeCheckBonus - only indices 2-5 (Knight=2, Bishop=3, Rook=4, Queen=5)
    params.push_back(TunableParam("SafeCheck_Knight", &Tuning::SafeCheckBonus[2], 10, 100, true));
    params.push_back(TunableParam("SafeCheck_Bishop", &Tuning::SafeCheckBonus[3], 10, 100, true));
    params.push_back(TunableParam("SafeCheck_Rook",   &Tuning::SafeCheckBonus[4], 10, 100, true));
    params.push_back(TunableParam("SafeCheck_Queen",  &Tuning::SafeCheckBonus[5], 10, 100, true));
    params.push_back(TunableParam("ContactCheckBonus", &Tuning::ContactCheckBonus, 10, 80, true));

    // KingTropismWeight - only indices 2-5
    params.push_back(TunableParam("KingTropism_Knight", &Tuning::KingTropismWeight[2], 0, 12, true));
    params.push_back(TunableParam("KingTropism_Bishop", &Tuning::KingTropismWeight[3], 0, 12, true));
    params.push_back(TunableParam("KingTropism_Rook",   &Tuning::KingTropismWeight[4], 0, 12, true));
    params.push_back(TunableParam("KingTropism_Queen",  &Tuning::KingTropismWeight[5], 0, 12, true));

    // PawnShieldQuality[5][4] - all 20 entries
    for (int f = 0; f < 5; f++) {
        for (int r = 0; r < 4; r++) {
            std::string name = "ShieldQ_f" + std::to_string(f) + "r" + std::to_string(r);
            params.push_back(TunableParam(name, &Tuning::PawnShieldQuality[f][r], -60, 30, true));
        }
    }

    // PawnStormDanger[5][4] - skip [*][0] (always 0), tune [*][1..3]
    for (int f = 0; f < 5; f++) {
        for (int r = 1; r < 4; r++) {
            std::string name = "StormD_f" + std::to_string(f) + "r" + std::to_string(r);
            params.push_back(TunableParam(name, &Tuning::PawnStormDanger[f][r], 0, 80, true));
        }
    }

    // ShelterWeakness[4] - skip [0] (always S(0,0)), tune [1..3]
    for (int i = 1; i <= 3; i++) {
        std::string istr = std::to_string(i);
        add_eval_param("ShelterWeak" + istr, Tuning::ShelterWeakness[i], -80, 0, -30, 0);
    }

    // ====================================================================
    // Piece Patterns (4 params)
    // ====================================================================
    add_eval_param("FianchettoBonus",       Tuning::FianchettoBonus,        0,  30,    0,  20);
    add_eval_param("TrappedKnightPenalty",  Tuning::TrappedKnightPenalty, -80,   0, -60,   0);

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
    std::cout << "\n=== Starting Texel Tuning (Fast Local Search v7) ===\n";
    std::cout << "Threads: " << NUM_THREADS << "\n";
    std::cout << "Iterations: " << iterations << "\n";
    std::cout << "Positions: " << positions.size() << "\n";
    std::cout << "Parameters: " << params.size() << "\n";
    std::cout << "Monotonic groups: " << monotonic_groups.size() << "\n";
    for (const auto& g : monotonic_groups) {
        std::cout << "  - " << g.name << " [" << g.start_idx << ".." << g.end_idx << "]\n";
    }
    std::cout << "\n";

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

                // Check monotonicity constraint
                if (!check_monotonicity()) {
                    *params[p].value_ptr = original;
                } else {
                    reevaluate_all_scores();
                    double error_up = calculate_error_fast(K);

                    if (error_up < best_error) {
                        best_error = error_up;
                        improved_this_iter = true;
                        params_changed++;
                        continue;
                    }
                    // Not better, restore
                    *params[p].value_ptr = original;
                }
            }

            // Try decreasing
            int new_val_down = std::clamp(original - step, params[p].min_val, params[p].max_val);
            if (new_val_down != original) {
                *params[p].value_ptr = new_val_down;

                // Check monotonicity constraint
                if (!check_monotonicity()) {
                    *params[p].value_ptr = original;
                } else {
                    reevaluate_all_scores();
                    double error_down = calculate_error_fast(K);

                    if (error_down < best_error) {
                        best_error = error_down;
                        improved_this_iter = true;
                        params_changed++;
                        continue;
                    }
                    // Not better, restore
                    *params[p].value_ptr = original;
                }
            }

            // Ensure original is restored and re-evaluate
            if (*params[p].value_ptr != original) {
                *params[p].value_ptr = original;
            }
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

        // Print current values periodically
        if (iter % 5 == 0 || !improved_this_iter) {
            std::cout << "\n--- Current Values (Iter " << iter << ") ---\n";
            for (const auto& param : params) {
                std::cout << "  " << std::setw(28) << std::left << param.name
                          << " = " << std::setw(5) << std::right << *param.value_ptr << "\n";
            }
            std::cout << "\n";
        }

        if (step == 1 && no_improvement_count >= 5) {
            std::cout << "Converged after " << iter << " iterations!\n";
            break;
        }
    }

    // ====================================================================
    // Print final values (ready to copy into tuning.cpp)
    // ====================================================================
    std::cout << "\n";
    std::cout << "// ================================================================\n";
    std::cout << "// FINAL TUNED VALUES - Copy to tuning.cpp\n";
    std::cout << "// ================================================================\n\n";

    // Material
    std::cout << "    EvalScore PawnValue   = S(" << std::setw(4) << Tuning::PawnValue.mg << ", " << std::setw(4) << Tuning::PawnValue.eg << ");\n";
    std::cout << "    EvalScore KnightValue = S(" << std::setw(4) << Tuning::KnightValue.mg << ", " << std::setw(4) << Tuning::KnightValue.eg << ");\n";
    std::cout << "    EvalScore BishopValue = S(" << std::setw(4) << Tuning::BishopValue.mg << ", " << std::setw(4) << Tuning::BishopValue.eg << ");\n";
    std::cout << "    EvalScore RookValue   = S(" << std::setw(4) << Tuning::RookValue.mg << ", " << std::setw(4) << Tuning::RookValue.eg << ");\n";
    std::cout << "    EvalScore QueenValue  = S(" << std::setw(4) << Tuning::QueenValue.mg << ", " << std::setw(4) << Tuning::QueenValue.eg << ");\n\n";

    // Piece Activity
    std::cout << "    EvalScore BishopPairBonus       = S(" << std::setw(4) << Tuning::BishopPairBonus.mg << ", " << std::setw(4) << Tuning::BishopPairBonus.eg << ");\n";
    std::cout << "    EvalScore RookOpenFileBonus     = S(" << std::setw(4) << Tuning::RookOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookOpenFileBonus.eg << ");\n";
    std::cout << "    EvalScore RookSemiOpenFileBonus = S(" << std::setw(4) << Tuning::RookSemiOpenFileBonus.mg << ", " << std::setw(4) << Tuning::RookSemiOpenFileBonus.eg << ");\n";
    std::cout << "    EvalScore RookOnSeventhBonus    = S(" << std::setw(4) << Tuning::RookOnSeventhBonus.mg << ", " << std::setw(4) << Tuning::RookOnSeventhBonus.eg << ");\n";
    std::cout << "    EvalScore KnightOutpostBonus    = S(" << std::setw(4) << Tuning::KnightOutpostBonus.mg << ", " << std::setw(4) << Tuning::KnightOutpostBonus.eg << ");\n\n";

    // Pawn Structure
    std::cout << "    EvalScore IsolatedPawnPenalty = S(" << std::setw(4) << Tuning::IsolatedPawnPenalty.mg << ", " << std::setw(4) << Tuning::IsolatedPawnPenalty.eg << ");\n";
    std::cout << "    EvalScore DoubledPawnPenalty  = S(" << std::setw(4) << Tuning::DoubledPawnPenalty.mg << ", " << std::setw(4) << Tuning::DoubledPawnPenalty.eg << ");\n";
    std::cout << "    EvalScore BackwardPawnPenalty = S(" << std::setw(4) << Tuning::BackwardPawnPenalty.mg << ", " << std::setw(4) << Tuning::BackwardPawnPenalty.eg << ");\n";
    std::cout << "    EvalScore ConnectedPawnBonus  = S(" << std::setw(4) << Tuning::ConnectedPawnBonus.mg << ", " << std::setw(4) << Tuning::ConnectedPawnBonus.eg << ");\n";
    std::cout << "    EvalScore PhalanxBonus        = S(" << std::setw(4) << Tuning::PhalanxBonus.mg << ", " << std::setw(4) << Tuning::PhalanxBonus.eg << ");\n";
    std::cout << "    EvalScore PawnIslandPenalty   = S(" << std::setw(4) << Tuning::PawnIslandPenalty.mg << ", " << std::setw(4) << Tuning::PawnIslandPenalty.eg << ");\n";
    std::cout << "    EvalScore PawnChainBonus      = S(" << std::setw(4) << Tuning::PawnChainBonus.mg << ", " << std::setw(4) << Tuning::PawnChainBonus.eg << ");\n";
    std::cout << "    EvalScore PawnChainBaseBonus  = S(" << std::setw(4) << Tuning::PawnChainBaseBonus.mg << ", " << std::setw(4) << Tuning::PawnChainBaseBonus.eg << ");\n";
    std::cout << "    EvalScore CentralPawnBonus    = S(" << std::setw(4) << Tuning::CentralPawnBonus.mg << ", " << std::setw(4) << Tuning::CentralPawnBonus.eg << ");\n";
    std::cout << "    EvalScore PawnDuoBonus        = S(" << std::setw(4) << Tuning::PawnDuoBonus.mg << ", " << std::setw(4) << Tuning::PawnDuoBonus.eg << ");\n";
    std::cout << "    EvalScore OutpostHolePenalty  = S(" << std::setw(4) << Tuning::OutpostHolePenalty.mg << ", " << std::setw(4) << Tuning::OutpostHolePenalty.eg << ");\n";
    std::cout << "    EvalScore HangingPawnPenalty  = S(" << std::setw(4) << Tuning::HangingPawnPenalty.mg << ", " << std::setw(4) << Tuning::HangingPawnPenalty.eg << ");\n";
    std::cout << "    EvalScore BackwardOnHalfOpen  = S(" << std::setw(4) << Tuning::BackwardOnHalfOpen.mg << ", " << std::setw(4) << Tuning::BackwardOnHalfOpen.eg << ");\n";
    std::cout << "    EvalScore CentralLeverBonus   = S(" << std::setw(4) << Tuning::CentralLeverBonus.mg << ", " << std::setw(4) << Tuning::CentralLeverBonus.eg << ");\n\n";

    // Passed Pawns
    std::cout << "    EvalScore PassedPawnBonus[8] = {\n        ";
    for (int i = 0; i < 8; i++) {
        std::cout << "S(" << std::setw(4) << Tuning::PassedPawnBonus[i].mg << "," << std::setw(4) << Tuning::PassedPawnBonus[i].eg << ")";
        if (i < 7) std::cout << ", ";
        if (i == 3) std::cout << "\n        ";
    }
    std::cout << "\n    };\n";

    std::cout << "    EvalScore ConnectedPassedBonus[8] = {\n        ";
    for (int i = 0; i < 8; i++) {
        std::cout << "S(" << std::setw(4) << Tuning::ConnectedPassedBonus[i].mg << "," << std::setw(4) << Tuning::ConnectedPassedBonus[i].eg << ")";
        if (i < 7) std::cout << ", ";
        if (i == 3) std::cout << "\n        ";
    }
    std::cout << "\n    };\n";

    std::cout << "    EvalScore ProtectedPassedBonus[8] = {\n        ";
    for (int i = 0; i < 8; i++) {
        std::cout << "S(" << std::setw(4) << Tuning::ProtectedPassedBonus[i].mg << "," << std::setw(4) << Tuning::ProtectedPassedBonus[i].eg << ")";
        if (i < 7) std::cout << ", ";
        if (i == 3) std::cout << "\n        ";
    }
    std::cout << "\n    };\n";

    std::cout << "    EvalScore CandidatePassedBonus[8] = {\n        ";
    for (int i = 0; i < 8; i++) {
        std::cout << "S(" << std::setw(4) << Tuning::CandidatePassedBonus[i].mg << "," << std::setw(4) << Tuning::CandidatePassedBonus[i].eg << ")";
        if (i < 7) std::cout << ", ";
        if (i == 3) std::cout << "\n        ";
    }
    std::cout << "\n    };\n";
    std::cout << "    int RuleOfSquareBonus = " << Tuning::RuleOfSquareBonus << ";\n\n";

    // Mobility Tables
    auto print_mob = [](const char* name, const EvalScore* table, int size) {
        std::cout << "    EvalScore " << name << "[" << size << "] = {\n        ";
        for (int i = 0; i < size; i++) {
            std::cout << "S(" << std::setw(4) << table[i].mg << "," << std::setw(4) << table[i].eg << ")";
            if (i < size - 1) std::cout << ", ";
            if ((i + 1) % 4 == 0 && i < size - 1) std::cout << "\n        ";
        }
        std::cout << "\n    };\n";
    };
    print_mob("KnightMobility", Tuning::KnightMobility, 9);
    print_mob("BishopMobility", Tuning::BishopMobility, 14);
    print_mob("RookMobility",   Tuning::RookMobility,   15);
    print_mob("QueenMobility",  Tuning::QueenMobility,  28);
    std::cout << "\n";

    // King Safety
    std::cout << "    int KingSafetyWeight = " << Tuning::KingSafetyWeight << ";\n";
    std::cout << "    int KnightAttackWeight = " << Tuning::KnightAttackWeight << ";\n";
    std::cout << "    int BishopAttackWeight = " << Tuning::BishopAttackWeight << ";\n";
    std::cout << "    int RookAttackWeight   = " << Tuning::RookAttackWeight << ";\n";
    std::cout << "    int QueenAttackWeight  = " << Tuning::QueenAttackWeight << ";\n";
    std::cout << "    int InnerRingAttackWeight = " << Tuning::InnerRingAttackWeight << ";\n";
    std::cout << "    int OuterRingAttackWeight = " << Tuning::OuterRingAttackWeight << ";\n";
    std::cout << "    EvalScore KingSemiOpenFilePenalty = S(" << std::setw(4) << Tuning::KingSemiOpenFilePenalty.mg << ", " << std::setw(4) << Tuning::KingSemiOpenFilePenalty.eg << ");\n";
    std::cout << "    EvalScore KingOpenFilePenalty     = S(" << std::setw(4) << Tuning::KingOpenFilePenalty.mg << ", " << std::setw(4) << Tuning::KingOpenFilePenalty.eg << ");\n";
    std::cout << "    int PawnShieldBonus[4] = { 0, " << Tuning::PawnShieldBonus[1] << ", " << Tuning::PawnShieldBonus[2] << ", " << Tuning::PawnShieldBonus[3] << " };\n\n";

    // Piece Activity
    std::cout << "    EvalScore KnightOnRim          = S(" << std::setw(4) << Tuning::KnightOnRim.mg << ", " << std::setw(4) << Tuning::KnightOnRim.eg << ");\n";
    std::cout << "    EvalScore BishopLongDiagonal   = S(" << std::setw(4) << Tuning::BishopLongDiagonal.mg << ", " << std::setw(4) << Tuning::BishopLongDiagonal.eg << ");\n";
    std::cout << "    EvalScore RookBehindPasser     = S(" << std::setw(4) << Tuning::RookBehindPasser.mg << ", " << std::setw(4) << Tuning::RookBehindPasser.eg << ");\n";
    std::cout << "    EvalScore TrappedBishopPenalty = S(" << std::setw(4) << Tuning::TrappedBishopPenalty.mg << ", " << std::setw(4) << Tuning::TrappedBishopPenalty.eg << ");\n";
    std::cout << "    EvalScore TrappedRookPenalty   = S(" << std::setw(4) << Tuning::TrappedRookPenalty.mg << ", " << std::setw(4) << Tuning::TrappedRookPenalty.eg << ");\n";
    std::cout << "    EvalScore HighMobilityBonus    = S(" << std::setw(4) << Tuning::HighMobilityBonus.mg << ", " << std::setw(4) << Tuning::HighMobilityBonus.eg << ");\n";
    std::cout << "    EvalScore LowMobilityPenalty   = S(" << std::setw(4) << Tuning::LowMobilityPenalty.mg << ", " << std::setw(4) << Tuning::LowMobilityPenalty.eg << ");\n\n";

    // Material Imbalance
    std::cout << "    EvalScore RookPairBonus        = S(" << std::setw(4) << Tuning::RookPairBonus.mg << ", " << std::setw(4) << Tuning::RookPairBonus.eg << ");\n";
    std::cout << "    EvalScore BishopKnightCombo    = S(" << std::setw(4) << Tuning::BishopKnightCombo.mg << ", " << std::setw(4) << Tuning::BishopKnightCombo.eg << ");\n";
    std::cout << "    EvalScore KnightPairPenalty    = S(" << std::setw(4) << Tuning::KnightPairPenalty.mg << ", " << std::setw(4) << Tuning::KnightPairPenalty.eg << ");\n";
    std::cout << "    EvalScore RooksWithoutQueens   = S(" << std::setw(4) << Tuning::RooksWithoutQueens.mg << ", " << std::setw(4) << Tuning::RooksWithoutQueens.eg << ");\n";
    std::cout << "    EvalScore QueenWithoutRooks    = S(" << std::setw(4) << Tuning::QueenWithoutRooks.mg << ", " << std::setw(4) << Tuning::QueenWithoutRooks.eg << ");\n\n";

    // King Safety Extended
    std::cout << "    int SafeCheckBonus[7] = {\n";
    std::cout << "        0, 0, " << Tuning::SafeCheckBonus[2] << ", " << Tuning::SafeCheckBonus[3]
              << ", " << Tuning::SafeCheckBonus[4] << ", " << Tuning::SafeCheckBonus[5] << ", 0\n";
    std::cout << "    };\n";
    std::cout << "    int ContactCheckBonus = " << Tuning::ContactCheckBonus << ";\n\n";

    std::cout << "    int KingTropismWeight[7] = {\n";
    std::cout << "        0, 0, " << Tuning::KingTropismWeight[2] << ", " << Tuning::KingTropismWeight[3]
              << ", " << Tuning::KingTropismWeight[4] << ", " << Tuning::KingTropismWeight[5] << ", 0\n";
    std::cout << "    };\n\n";

    std::cout << "    int PawnShieldQuality[5][4] = {\n";
    for (int f = 0; f < 5; f++) {
        std::cout << "        { " << std::setw(4) << Tuning::PawnShieldQuality[f][0]
                  << ", " << std::setw(4) << Tuning::PawnShieldQuality[f][1]
                  << ", " << std::setw(4) << Tuning::PawnShieldQuality[f][2]
                  << ", " << std::setw(4) << Tuning::PawnShieldQuality[f][3] << " },\n";
    }
    std::cout << "    };\n\n";

    std::cout << "    int PawnStormDanger[5][4] = {\n";
    for (int f = 0; f < 5; f++) {
        std::cout << "        { " << std::setw(4) << Tuning::PawnStormDanger[f][0]
                  << ", " << std::setw(4) << Tuning::PawnStormDanger[f][1]
                  << ", " << std::setw(4) << Tuning::PawnStormDanger[f][2]
                  << ", " << std::setw(4) << Tuning::PawnStormDanger[f][3] << " },\n";
    }
    std::cout << "    };\n\n";

    std::cout << "    EvalScore ShelterWeakness[4] = {\n        ";
    for (int i = 0; i < 4; i++) {
        std::cout << "S(" << std::setw(4) << Tuning::ShelterWeakness[i].mg << "," << std::setw(4) << Tuning::ShelterWeakness[i].eg << ")";
        if (i < 3) std::cout << ", ";
    }
    std::cout << "\n    };\n\n";

    // Piece Patterns
    std::cout << "    EvalScore FianchettoBonus      = S(" << std::setw(4) << Tuning::FianchettoBonus.mg << ", " << std::setw(4) << Tuning::FianchettoBonus.eg << ");\n";
    std::cout << "    EvalScore TrappedKnightPenalty = S(" << std::setw(4) << Tuning::TrappedKnightPenalty.mg << ", " << std::setw(4) << Tuning::TrappedKnightPenalty.eg << ");\n\n";

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
    std::cout << "  GC-Engine Texel Tuner v7\n";
    std::cout << "  (Extended ~200 Parameters)\n";
    std::cout << "=================================\n\n";
    std::cout << "Usage: tuner.exe <epd_file> [max_positions] [iterations] [manual_K] [threads]\n";
    std::cout << "  epd_file      : Path to labeled EPD file\n";
    std::cout << "  max_positions : Maximum positions to load (0 = all)\n";
    std::cout << "  iterations    : Number of tuning iterations (default: 100)\n";
    std::cout << "  manual_K      : Optional manual K value (default: auto-find, 0 = auto)\n";
    std::cout << "  threads       : Number of threads (default: auto-detect)\n\n";

    // Override thread count if provided as 5th argument
    if (argc > 5) {
        unsigned int requested = std::stoi(argv[5]);
        if (requested > 0) NUM_THREADS = requested;
    }
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
