#ifndef TESTS_HPP
#define TESTS_HPP

#include "types.hpp"

// ============================================================================
// Test Suite Header
// ============================================================================
// Contains declarations for all test functions.
// Implementation is in tests.cpp (excluded from normal builds).
// ============================================================================

namespace Tests {

// Stage 1 tests (basic functionality)
void test_bitboards();
void test_magic_bitboards();

// Stage 2 tests (core engine components)
void test_move_generation();
void test_transposition_table();
void test_see();
void test_move_ordering();
void test_evaluation();
void test_perft();
void test_search();

// Test runners
void run_all_tests();
void run_benchmark();
void run_perft(int maxDepth = 5);

// WAC Test Suite
struct WACResult {
    int total = 0;
    int passed = 0;
    int failed = 0;
    int errors = 0;
    U64 totalNodes = 0;
    U64 totalTimeMs = 0;

    double pass_rate() const {
        return total > 0 ? (100.0 * passed / total) : 0.0;
    }
};

// Run WAC test suite
// depth: search depth (0 = use time limit)
// timeLimitMs: time limit per position in milliseconds (0 = use depth)
// startPos: starting position index (1-based)
// endPos: ending position index (1-based, 0 = all)
// verbose: print each position result
WACResult run_wac_test(int depth = 10, int timeLimitMs = 0,
                       int startPos = 1, int endPos = 0,
                       bool verbose = true);

// Print help
void print_help();

} // namespace Tests

#endif // TESTS_HPP
