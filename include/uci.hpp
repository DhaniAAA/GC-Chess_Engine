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

namespace UCI {

struct EngineOptions {
    int hash = 256;
    int threads = 2;
    int multiPV = 1;
    bool ponder = true;
    std::string bookPath = "";
    std::string syzygyPath = "";
    int moveOverhead = 10;

    int contempt = 20;
    bool dynamicContempt = true;

    U64 ponderHits = 0;
    U64 ponderAttempts = 0;
    Move lastPonderMove = MOVE_NONE;
};
extern EngineOptions options;

class UCIHandler {
public:
    UCIHandler();
    ~UCIHandler();

    void loop();

private:
    Board board;
    std::thread searchThread;
    std::atomic<bool> searching;

    std::string ponderFen;
    Move expectedPonderMove;
    bool isPondering = false;
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

    void parse_moves(std::istringstream& is);
    void start_search(const SearchLimits& limits);
    void wait_for_search();
};

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

}

#endif
