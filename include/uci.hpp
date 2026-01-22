#ifndef UCI_HPP
#define UCI_HPP

#include "board.hpp"
#include "search.hpp"
#include "book.hpp"
#include "tablebase.hpp"
#include <string>
#include <sstream>
#include <thread>
#include <atomic>

// ============================================================================
// Universal Chess Interface (UCI) Protocol Implementation (TAHAP 6)
//
// UCI is the standard protocol for chess engine communication.
// Commands:
//   - uci: Engine identification
//   - isready: Synchronization
//   - ucinewgame: Reset for new game
//   - position: Set up position
//   - go: Start searching
//   - stop: Stop searching
//   - quit: Exit engine
//   - setoption: Configure engine options
// ============================================================================

namespace UCI {

// ============================================================================
// Engine Options
// ============================================================================

struct EngineOptions {
    int hash = 256;
    int threads = 2;
    int multiPV = 1;
    bool ponder = true;
    std::string bookPath = "";
    std::string syzygyPath = "";
    int moveOverhead = 10;

    // Contempt Factor
    int contempt = 20;
    bool dynamicContempt = true;

    // Ponder Statistics (for tracking hit rate)
    U64 ponderHits = 0;
    U64 ponderAttempts = 0;
    Move lastPonderMove = MOVE_NONE;
};

// Global options
extern EngineOptions options;

// ============================================================================
// UCI Main Loop
// ============================================================================

class UCIHandler {
public:
    UCIHandler();
    ~UCIHandler();

    // Main loop - processes UCI commands
    void loop();

private:
    Board board;
    std::thread searchThread;
    std::atomic<bool> searching;

    // Ponder state
    std::string ponderFen;
    Move expectedPonderMove;
    bool isPondering = false;

    // Command handlers
    void cmd_uci();
    void cmd_isready();
    void cmd_ucinewgame();
    void cmd_position(std::istringstream& is);
    void cmd_go(std::istringstream& is);
    void cmd_stop();
    void cmd_ponderhit();
    void cmd_quit();
    void cmd_setoption(std::istringstream& is);
    void cmd_perft(std::istringstream& is);
    void cmd_divide(std::istringstream& is);
    void cmd_d();
    void cmd_eval();
    void cmd_bench(std::istringstream& is);
    void cmd_datagen(std::istringstream& is);

    // Helper functions
    void parse_moves(std::istringstream& is);
    void start_search(const SearchLimits& limits);
    void wait_for_search();
};

// ============================================================================
// Time Management
// ============================================================================

class TimeManager {
public:
    TimeManager();

    void init(Color us, int timeLeft, int increment, int movesToGo, int moveTime);

    int optimal_time() const { return optimalTime; }
    int maximum_time() const { return maximumTime; }
    bool should_stop(int elapsed, int depth, bool bestMoveStable);
    void adjust(bool scoreDropped, bool bestMoveChanged);

private:
    int optimalTime;
    int maximumTime;
    int startTime;
    int incrementTime;
    int movesToGo;
    double stability;
};

extern TimeManager timeMgr;

} // namespace UCI

#endif // UCI_HPP
