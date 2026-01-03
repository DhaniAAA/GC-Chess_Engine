// ============================================================================
// profiler.hpp - Internal Performance Profiler for Chess Engine
// ============================================================================
// Usage:
//   1. Build with -DPROFILING to enable profiling (or define PROFILING below)
//   2. Use PROFILE_SCOPE("name") at start of functions to measure
//   3. Call Profiler::print_results() to see the breakdown
// ============================================================================

#ifndef PROFILER_HPP
#define PROFILER_HPP

#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <atomic>

// ============================================================================
// Enable/Disable Profiling
// ============================================================================
// Uncomment the line below to enable profiling, or build with -DPROFILING
// #define PROFILING

#ifdef PROFILING
    #define PROFILE_SCOPE(name) ProfilerScope __profiler_scope__(name)
    #define PROFILE_FUNCTION() ProfilerScope __profiler_scope__(__FUNCTION__)
#else
    #define PROFILE_SCOPE(name)
    #define PROFILE_FUNCTION()
#endif

// ============================================================================
// Profiler Data Structures
// ============================================================================

struct ProfileData {
    std::atomic<uint64_t> totalTimeNs{0};   // Total time in nanoseconds
    std::atomic<uint64_t> callCount{0};      // Number of calls
    std::atomic<uint64_t> maxTimeNs{0};      // Maximum single call time
    std::atomic<uint64_t> minTimeNs{UINT64_MAX}; // Minimum single call time
};

// ============================================================================
// Profiler Class (Singleton)
// ============================================================================

class Profiler {
public:
    static Profiler& instance() {
        static Profiler profiler;
        return profiler;
    }

    // Record a profile entry
    void record(const std::string& name, uint64_t durationNs) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& data = profiles_[name];
        data.totalTimeNs.fetch_add(durationNs, std::memory_order_relaxed);
        data.callCount.fetch_add(1, std::memory_order_relaxed);

        // Update max (atomic compare-exchange loop)
        uint64_t currentMax = data.maxTimeNs.load(std::memory_order_relaxed);
        while (durationNs > currentMax &&
               !data.maxTimeNs.compare_exchange_weak(currentMax, durationNs,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}

        // Update min (atomic compare-exchange loop)
        uint64_t currentMin = data.minTimeNs.load(std::memory_order_relaxed);
        while (durationNs < currentMin &&
               !data.minTimeNs.compare_exchange_weak(currentMin, durationNs,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    // Print profiling results
    static void print_results() {
        auto& profiler = instance();
        std::lock_guard<std::mutex> lock(profiler.mutex_);

        if (profiler.profiles_.empty()) {
            std::cout << "\n[Profiler] No profiling data collected.\n";
            std::cout << "Make sure PROFILING is defined and PROFILE_SCOPE/PROFILE_FUNCTION macros are used.\n";
            return;
        }

        // Calculate total time
        uint64_t totalTime = 0;
        for (const auto& [name, data] : profiler.profiles_) {
            totalTime += data.totalTimeNs.load();
        }

        // Sort by total time (descending)
        std::vector<std::pair<std::string, ProfileData*>> sorted;
        for (auto& [name, data] : profiler.profiles_) {
            sorted.push_back({name, &data});
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                return a.second->totalTimeNs.load() > b.second->totalTimeNs.load();
            });

        // Print header
        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "                           PROFILING RESULTS\n";
        std::cout << "================================================================================\n";
        std::cout << std::left << std::setw(30) << "Function"
                  << std::right << std::setw(10) << "% Time"
                  << std::setw(12) << "Total(ms)"
                  << std::setw(12) << "Calls"
                  << std::setw(12) << "Avg(us)"
                  << std::setw(12) << "Max(us)"
                  << "\n";
        std::cout << "--------------------------------------------------------------------------------\n";

        // Print data
        for (const auto& [name, dataPtr] : sorted) {
            uint64_t total = dataPtr->totalTimeNs.load();
            uint64_t calls = dataPtr->callCount.load();
            uint64_t maxNs = dataPtr->maxTimeNs.load();

            double percent = totalTime > 0 ? (total * 100.0 / totalTime) : 0.0;
            double totalMs = total / 1e6;
            double avgUs = calls > 0 ? (total / 1000.0 / calls) : 0.0;
            double maxUs = maxNs / 1000.0;

            // Truncate long names
            std::string displayName = name;
            if (displayName.length() > 28) {
                displayName = displayName.substr(0, 25) + "...";
            }

            std::cout << std::left << std::setw(30) << displayName
                      << std::right << std::fixed << std::setprecision(1)
                      << std::setw(9) << percent << "%"
                      << std::setprecision(1) << std::setw(12) << totalMs
                      << std::setw(12) << calls
                      << std::setprecision(2) << std::setw(12) << avgUs
                      << std::setprecision(1) << std::setw(12) << maxUs
                      << "\n";
        }

        std::cout << "--------------------------------------------------------------------------------\n";
        std::cout << "Total profiled time: " << std::fixed << std::setprecision(1)
                  << (totalTime / 1e9) << " seconds\n";
        std::cout << "================================================================================\n\n";
    }

    // Reset all profiling data
    static void reset() {
        auto& profiler = instance();
        std::lock_guard<std::mutex> lock(profiler.mutex_);
        profiler.profiles_.clear();
    }

private:
    Profiler() = default;
    std::unordered_map<std::string, ProfileData> profiles_;
    std::mutex mutex_;
};

// ============================================================================
// RAII Scope Timer
// ============================================================================

class ProfilerScope {
public:
    explicit ProfilerScope(const char* name)
        : name_(name)
        , start_(std::chrono::high_resolution_clock::now()) {}

    ~ProfilerScope() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        Profiler::instance().record(name_, duration);
    }

private:
    const char* name_;
    std::chrono::high_resolution_clock::time_point start_;
};

// ============================================================================
// Bottleneck Analysis Helper
// ============================================================================

namespace ProfilerAnalysis {

inline void analyze_bottlenecks() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "                         BOTTLENECK ANALYSIS\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";
    std::cout << "Key areas to check for chess engine bottlenecks:\n";
    std::cout << "\n";
    std::cout << "1. MOVE GENERATION (generate_*, movegen)\n";
    std::cout << "   - Target: < 15% of total time\n";
    std::cout << "   - If high: Consider lazy generation, incremental updates\n";
    std::cout << "\n";
    std::cout << "2. EVALUATION (evaluate, eval)\n";
    std::cout << "   - Target: < 20% of total time\n";
    std::cout << "   - If high: Implement lazy evaluation, NNUE, or simpler eval\n";
    std::cout << "\n";
    std::cout << "3. TRANSPOSITION TABLE (tt_probe, tt_store)\n";
    std::cout << "   - Target: < 5% of total time\n";
    std::cout << "   - If high: Check cache alignment, hash function efficiency\n";
    std::cout << "\n";
    std::cout << "4. MOVE ORDERING (score_moves, pick_move)\n";
    std::cout << "   - Target: < 10% of total time\n";
    std::cout << "   - If high: Use incremental scoring, avoid full sort\n";
    std::cout << "\n";
    std::cout << "5. POSITION UPDATES (do_move, undo_move)\n";
    std::cout << "   - Target: < 10% of total time\n";
    std::cout << "   - If high: Use incremental updates, copy-make vs make-unmake\n";
    std::cout << "\n";
    std::cout << "6. SEE (static_exchange_evaluation)\n";
    std::cout << "   - Target: < 5% of total time\n";
    std::cout << "   - If high: Cache SEE results, optimize attacker lookups\n";
    std::cout << "\n";
    std::cout << "Expected NPS for modern engines:\n";
    std::cout << "   - Basic engine: 500K - 1M NPS\n";
    std::cout << "   - Optimized: 1M - 5M NPS\n";
    std::cout << "   - Top engines (Stockfish): 10M+ NPS\n";
    std::cout << "\n";
    std::cout << "================================================================================\n";
}

} // namespace ProfilerAnalysis

#endif // PROFILER_HPP
