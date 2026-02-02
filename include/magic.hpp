#ifndef MAGIC_HPP
#define MAGIC_HPP

#include "bitboard.hpp"

#ifdef USE_PEXT
#include "pext.hpp"
#endif

struct Magic {
    Bitboard  mask;
    Bitboard  magic;
    Bitboard* attacks;
    int       shift;

    unsigned index(Bitboard occupied) const {
#ifdef USE_PEXT
        return static_cast<unsigned>(pext(occupied, mask));
#else
        return static_cast<unsigned>(((occupied & mask) * magic) >> shift);
#endif
    }
};

extern Magic BishopMagics[SQUARE_NB];
extern Magic RookMagics[SQUARE_NB];

inline Bitboard bishop_attacks_bb(Square s, Bitboard occupied) {
    return BishopMagics[s].attacks[BishopMagics[s].index(occupied)];
}

inline Bitboard rook_attacks_bb(Square s, Bitboard occupied) {
    return RookMagics[s].attacks[RookMagics[s].index(occupied)];
}

inline Bitboard queen_attacks_bb(Square s, Bitboard occupied) {
    return bishop_attacks_bb(s, occupied) | rook_attacks_bb(s, occupied);
}

inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt) {
        case BISHOP: return bishop_attacks_bb(s, occupied);
        case ROOK:   return rook_attacks_bb(s, occupied);
        case QUEEN:  return queen_attacks_bb(s, occupied);
        case KNIGHT: return knight_attacks_bb(s);
        case KING:   return king_attacks_bb(s);
        default:     return EMPTY_BB;
    }
}

namespace Magics {
    void init();
}

#endif
