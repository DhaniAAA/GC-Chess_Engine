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
    int hash = 256;             // Hash size in MB
    int threads = 2;            // Number of threads
    int multiPV = 1;            // Number of principal variations
    bool ponder = true;        // Pondering enabled by default
    std::string bookPath = "";  // Opening book path
    std::string syzygyPath = ""; // Syzygy tablebase path
    int moveOverhead = 10;      // Time overhead per move (ms)

    // Contempt Factor
    int contempt = 20;          // Contempt value in centipawns (positive = avoid draws)
    bool dynamicContempt = true; // Adjust contempt based on position

    // Ponder Statistics (for tracking hit rate)
    U64 ponderHits = 0;         // Times opponent played predicted move
    U64 ponderAttempts = 0;     // Total ponder attempts
    Move lastPonderMove = MOVE_NONE; // Last ponder move (to verify hit)
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
    std::string ponderFen;        // FEN after best move (where ponder move starts)
    Move expectedPonderMove;      // The ponder move we outputted
    bool isPondering = false;     // Currently in ponder mode

    // Command handlers
    void cmd_uci();
    void cmd_isready();
    void cmd_ucinewgame();
    void cmd_position(std::istringstream& is);
    void cmd_go(std::istringstream& is);
    void cmd_stop();
    void cmd_ponderhit();         // Handle ponderhit command
    void cmd_quit();
    void cmd_setoption(std::istringstream& is);
    void cmd_perft(std::istringstream& is);
    void cmd_divide(std::istringstream& is);  // Debugging command
    void cmd_d();  // Display board (debug)
    void cmd_eval(); // Display evaluation (debug)

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

    // Initialize time management for a search
    void init(Color us, int timeLeft, int increment, int movesToGo, int moveTime);

    // Get time limits
    int optimal_time() const { return optimalTime; }
    int maximum_time() const { return maximumTime; }

    // Check if we should stop based on time
    bool should_stop(int elapsed, int depth, bool bestMoveStable);

    // Adjust time based on search progress
    void adjust(bool scoreDropped, bool bestMoveChanged);

private:
    int optimalTime;    // Target time to use
    int maximumTime;    // Hard time limit
    int startTime;
    int incrementTime;
    int movesToGo;
    double stability;   // How stable the best move is (0.5 - 2.0)
};

extern TimeManager timeMgr;

} // namespace UCI

#endif // UCI_HPP
