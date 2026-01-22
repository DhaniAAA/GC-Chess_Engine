#ifndef MAGIC_HPP
#define MAGIC_HPP

#include "bitboard.hpp"

// Optional: Include PEXT for BMI2 support
#ifdef USE_PEXT
#include "pext.hpp"
#endif

// ============================================================================
// Magic Bitboard Structure
// Each entry contains pre-computed magic for O(1) attack lookup
//
// When USE_PEXT is defined, we use the PEXT instruction for index calculation.
// PEXT provides faster lookups on CPUs with BMI2 (Intel Haswell+, AMD Zen 3+).
// ============================================================================

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

// ============================================================================
// Sliding Piece Attack Functions
// ============================================================================

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

// ============================================================================
// Initialization
// ============================================================================

namespace Magics {
    void init();
}

#endif // MAGIC_HPP
