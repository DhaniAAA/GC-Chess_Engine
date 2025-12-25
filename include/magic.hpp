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
    Bitboard  mask;      // Relevant occupancy mask (excludes edges)
    Bitboard  magic;     // Magic number for hashing (unused with PEXT)
    Bitboard* attacks;   // Pointer to attack table
    int       shift;     // Bits to shift (64 - popcount(mask))

    // Get the index into the attack table
    unsigned index(Bitboard occupied) const {
#ifdef USE_PEXT
        // PEXT-based lookup (faster on BMI2-capable CPUs)
        return static_cast<unsigned>(pext(occupied, mask));
#else
        // Traditional magic multiplication
        return static_cast<unsigned>(((occupied & mask) * magic) >> shift);
#endif
    }
};

// Magic tables for bishops and rooks
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

// Attack generation based on piece type
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
