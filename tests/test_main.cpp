// ============================================================================
// test_main.cpp - Main entry point for unit tests
// ============================================================================

#include "../include/bitboard.hpp"
#include "../include/magic.hpp"
#include "../include/zobrist.hpp"
#include "../include/board.hpp"

#include <iostream>
#include <vector>
#include <functional>
#include <string>

// ============================================================================
// Simple Test Framework
// ============================================================================

struct TestCase {
    std::string name;
    std::string tags;
    std::function<void()> func;
};

static std::vector<TestCase>& getTests() {
    static std::vector<TestCase> tests;
    return tests;
}

static int passed = 0;
static int failed = 0;

struct TestRegistrar {
    TestRegistrar(const char* name, const char* tags, std::function<void()> func) {
        getTests().push_back({name, tags, func});
    }
};

#define TEST_CASE_IMPL2(name, tags, counter) \
    static void test_func_##counter(); \
    static TestRegistrar test_reg_##counter(name, tags, test_func_##counter); \
    static void test_func_##counter()

#define TEST_CASE_IMPL(name, tags, counter) TEST_CASE_IMPL2(name, tags, counter)
#define TEST_CASE(name, tags) TEST_CASE_IMPL(name, tags, __COUNTER__)

#define SECTION(name) if(true)

#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "  FAILED: " << #expr << " at line " << __LINE__ << "\n"; \
            failed++; \
            return; \
        } else { \
            passed++; \
        } \
    } while(false)

#define REQUIRE_FALSE(expr) REQUIRE(!(expr))
#define CHECK(expr) REQUIRE(expr)
#define CHECK_FALSE(expr) REQUIRE_FALSE(expr)
#define INFO(msg) (void)0

// ============================================================================
// Include all test files
// ============================================================================

#include "test_types.cpp"
#include "test_bitboard.cpp"
#include "test_move.cpp"
#include "test_board.cpp"
#include "test_movegen.cpp"

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "===============================================================================\n";
    std::cout << "GC-Chess Engine Unit Tests\n";
    std::cout << "===============================================================================\n\n";

    // Initialize engine lookup tables FIRST
    std::cout << "Initializing engine...\n";
    Bitboards::init();
    Magics::init();
    Zobrist::init();
    Position::init();
    std::cout << "Engine initialized.\n\n";

    // Run all tests
    int testCount = 0;
    int testsPassed = 0;

    for (auto& tc : getTests()) {
        passed = 0;
        failed = 0;

        std::cout << "Running: " << tc.name << " " << tc.tags << "\n";
        try {
            tc.func();
            if (failed == 0) {
                testsPassed++;
                std::cout << "  PASSED (" << passed << " assertions)\n";
            } else {
                std::cout << "  FAILED (" << failed << " failures, " << passed << " passed)\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "  UNKNOWN EXCEPTION\n";
        }
        testCount++;
    }

    std::cout << "\n===============================================================================\n";
    std::cout << "Results: " << testsPassed << "/" << testCount << " tests passed\n";
    std::cout << "===============================================================================\n";

    return (testsPassed == testCount) ? 0 : 1;
}
