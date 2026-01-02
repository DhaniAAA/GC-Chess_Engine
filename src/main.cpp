#include <iostream>
#include "board.hpp"
#include "bitboard.hpp"
#include "magic.hpp"
#include "zobrist.hpp"
#include "tt.hpp"
#include "uci.hpp"

void init_engine() {
    Bitboards::init();
    Magics::init();
    Zobrist::init();
    Position::init();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout.setf(std::ios::unitbuf);
    init_engine();

    UCI::UCIHandler uci;
    uci.loop();

    return 0;
}
