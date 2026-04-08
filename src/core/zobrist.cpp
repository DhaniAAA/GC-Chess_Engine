#include "zobrist.hpp"
#include <random>

namespace Zobrist {

Key PieceSquare[PIECE_NB][SQUARE_NB];
Key Castling[CASTLING_RIGHT_NB];
Key EnPassant[FILE_NB];
Key SideToMove;

namespace {

class PRNG {
public:
    explicit PRNG(U64 seed) : state(seed) {}

    U64 next() {
        U64 z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    U64 sparse() {
        return next() & next() & next();
    }

private:
    U64 state;
};

}

void init() {
    PRNG rng(1070372ULL);

    for (Piece pc = NO_PIECE; pc < PIECE_NB; ++pc) {
        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
            if (pc != NO_PIECE && type_of(pc) != NO_PIECE_TYPE) {
                PieceSquare[pc][sq] = rng.next();
            } else {
                PieceSquare[pc][sq] = 0;
            }
        }
    }

    for (int cr = 0; cr < CASTLING_RIGHT_NB; ++cr) {
        Castling[cr] = 0;

        Key k = 0;
        if (cr & WHITE_OO)  k ^= rng.next();
        else rng.next();

        if (cr & WHITE_OOO) k ^= rng.next();
        else rng.next();

        if (cr & BLACK_OO)  k ^= rng.next();
        else rng.next();

        if (cr & BLACK_OOO) k ^= rng.next();
        else rng.next();

        Castling[cr] = k;
    }

    Key castling_right_keys[4];
    rng = PRNG(31337ULL);

    for (int i = 0; i < 4; ++i) {
        castling_right_keys[i] = rng.next();
    }

    for (int cr = 0; cr < CASTLING_RIGHT_NB; ++cr) {
        Castling[cr] = 0;
        if (cr & WHITE_OO)  Castling[cr] ^= castling_right_keys[0];
        if (cr & WHITE_OOO) Castling[cr] ^= castling_right_keys[1];
        if (cr & BLACK_OO)  Castling[cr] ^= castling_right_keys[2];
        if (cr & BLACK_OOO) Castling[cr] ^= castling_right_keys[3];
    }

    for (File f = FILE_A; f <= FILE_H; ++f) {
        EnPassant[f] = rng.next();
    }

    SideToMove = rng.next();
}

}
