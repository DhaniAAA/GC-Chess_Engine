// Catch2 single header - minimal version for testing
// For full version, download from: https://github.com/catchorg/Catch2

#ifndef CATCH_HPP_INCLUDED
#define CATCH_HPP_INCLUDED

#define CATCH_VERSION_MAJOR 2
#define CATCH_VERSION_MINOR 13
#define CATCH_VERSION_PATCH 10

#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <functional>
#include <cmath>

namespace Catch {

// Forward declarations
struct TestCaseInfo;
class IConfig;

// Simple result tracking
struct Counts {
    int passed = 0;
    int failed = 0;
    int total() const { return passed + failed; }
};

// Global test state
inline Counts& testCounts() {
    static Counts counts;
    return counts;
}

// Source line info
struct SourceLineInfo {
    const char* file;
    int line;
    SourceLineInfo(const char* f, int l) : file(f), line(l) {}
};

// Test case registration
struct TestCase {
    std::string name;
    std::string tags;
    std::function<void()> func;
    SourceLineInfo lineInfo;

    TestCase(const std::string& n, const std::string& t, std::function<void()> f, SourceLineInfo l)
        : name(n), tags(t), func(f), lineInfo(l) {}
};

inline std::vector<TestCase>& getTestCases() {
    static std::vector<TestCase> testCases;
    return testCases;
}

struct AutoReg {
    AutoReg(const std::string& name, const std::string& tags, std::function<void()> func, SourceLineInfo lineInfo) {
        getTestCases().push_back(TestCase(name, tags, func, lineInfo));
    }
};

// Assertion helpers
inline void reportFailure(const char* file, int line, const std::string& expr, const std::string& expanded) {
    std::cerr << "\n" << file << ":" << line << ": FAILED:\n"
              << "  " << expr << "\n"
              << "with expansion:\n"
              << "  " << expanded << "\n";
    testCounts().failed++;
}

inline void reportSuccess() {
    testCounts().passed++;
}

// Info for current section
inline std::string& currentSection() {
    static std::string section;
    return section;
}

// Section support
struct Section {
    std::string name;
    bool entered = false;

    Section(const std::string& n) : name(n) {
        currentSection() = n;
        entered = true;
    }

    ~Section() {
        currentSection().clear();
    }

    operator bool() const { return entered; }
};

} // namespace Catch

// Macros for test definition
#define INTERNAL_CATCH_UNIQUE_NAME_LINE2(name, line) name##line
#define INTERNAL_CATCH_UNIQUE_NAME_LINE(name, line) INTERNAL_CATCH_UNIQUE_NAME_LINE2(name, line)
#define INTERNAL_CATCH_UNIQUE_NAME(name) INTERNAL_CATCH_UNIQUE_NAME_LINE(name, __LINE__)

#define TEST_CASE(name, tags) \
    static void INTERNAL_CATCH_UNIQUE_NAME(catch_test_)(); \
    static Catch::AutoReg INTERNAL_CATCH_UNIQUE_NAME(catch_autoreg_)( \
        name, tags, &INTERNAL_CATCH_UNIQUE_NAME(catch_test_), \
        Catch::SourceLineInfo(__FILE__, __LINE__)); \
    static void INTERNAL_CATCH_UNIQUE_NAME(catch_test_)()

#define SECTION(name) \
    if (Catch::Section INTERNAL_CATCH_UNIQUE_NAME(catch_section_) = Catch::Section(name))

// Assertion macros
#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            Catch::reportFailure(__FILE__, __LINE__, #expr, "false"); \
        } else { \
            Catch::reportSuccess(); \
        } \
    } while(false)

#define REQUIRE_FALSE(expr) \
    do { \
        if (expr) { \
            Catch::reportFailure(__FILE__, __LINE__, "!(" #expr ")", "true"); \
        } else { \
            Catch::reportSuccess(); \
        } \
    } while(false)

#define CHECK(expr) REQUIRE(expr)
#define CHECK_FALSE(expr) REQUIRE_FALSE(expr)

#define REQUIRE_THAT(expr, matcher) REQUIRE(expr)
#define CHECK_THAT(expr, matcher) CHECK(expr)

// INFO macro
#define INFO(msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
    } while(false)

// CATCH_CONFIG_MAIN implementation
#ifdef CATCH_CONFIG_MAIN

int main(int argc, char* argv[]) {
    std::cout << "===============================================================================\n";
    std::cout << "GC-Chess Engine Unit Tests (Catch2)\n";
    std::cout << "===============================================================================\n\n";

    for (auto& tc : Catch::getTestCases()) {
        std::cout << "Running: " << tc.name << " " << tc.tags << "\n";
        try {
            tc.func();
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
            Catch::testCounts().failed++;
        }
    }

    std::cout << "\n===============================================================================\n";
    std::cout << "Test Results: ";
    if (Catch::testCounts().failed == 0) {
        std::cout << "\033[32mAll " << Catch::testCounts().passed << " assertions passed\033[0m\n";
    } else {
        std::cout << "\033[31m" << Catch::testCounts().failed << " failed, "
                  << Catch::testCounts().passed << " passed\033[0m\n";
    }
    std::cout << "===============================================================================\n";

    return Catch::testCounts().failed > 0 ? 1 : 0;
}

#endif // CATCH_CONFIG_MAIN

#endif // CATCH_HPP_INCLUDED
