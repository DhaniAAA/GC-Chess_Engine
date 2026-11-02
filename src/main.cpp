#include "board.hpp"
#include "tt.hpp"
#include "uci.hpp"
#include "nnue.hpp"

void init_engine() {
    // Single canonical init path. Position::init() covers
    // Bitboards/Magics/Zobrist deterministically (same seeds each run).
    Position::init();

    NNUE::init();
    NNUE::load_eval_file(UCI::options.evalFile);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    init_engine();

    UCI::UCIHandler uci;
    uci.loop();

    return 0;
}
