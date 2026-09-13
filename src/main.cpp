#include "board.hpp"
#include "tt.hpp"
#include "uci.hpp"

void init_engine() {
    // Single canonical init path. Position::init() covers
    // Bitboards/Magics/Zobrist deterministically (same seeds each run).
    Position::init();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    init_engine();

    UCI::UCIHandler uci;
    uci.loop();

    return 0;
}
