#ifndef BITBOARD_HPP
#define BITBOARD_HPP

#include "types.hpp"
#include <string>

using Bitboard = U64;

constexpr Bitboard EMPTY_BB = 0ULL;
constexpr Bitboard FULL_BB  = ~0ULL;

constexpr Bitboard FILE_A_BB = 0x0101010101010101ULL;
constexpr Bitboard FILE_B_BB = FILE_A_BB << 1;
constexpr Bitboard FILE_C_BB = FILE_A_BB << 2;
constexpr Bitboard FILE_D_BB = FILE_A_BB << 3;
constexpr Bitboard FILE_E_BB = FILE_A_BB << 4;
constexpr Bitboard FILE_F_BB = FILE_A_BB << 5;
constexpr Bitboard FILE_G_BB = FILE_A_BB << 6;
constexpr Bitboard FILE_H_BB = FILE_A_BB << 7;

constexpr Bitboard RANK_1_BB = 0x00000000000000FFULL;
constexpr Bitboard RANK_2_BB = RANK_1_BB << (8 * 1);
constexpr Bitboard RANK_3_BB = RANK_1_BB << (8 * 2);
constexpr Bitboard RANK_4_BB = RANK_1_BB << (8 * 3);
constexpr Bitboard RANK_5_BB = RANK_1_BB << (8 * 4);
constexpr Bitboard RANK_6_BB = RANK_1_BB << (8 * 5);
constexpr Bitboard RANK_7_BB = RANK_1_BB << (8 * 6);
constexpr Bitboard RANK_8_BB = RANK_1_BB << (8 * 7);

constexpr Bitboard NOT_FILE_A_BB = ~FILE_A_BB;
constexpr Bitboard NOT_FILE_H_BB = ~FILE_H_BB;
constexpr Bitboard NOT_FILE_AB_BB = ~(FILE_A_BB | FILE_B_BB);
constexpr Bitboard NOT_FILE_GH_BB = ~(FILE_G_BB | FILE_H_BB);

extern Bitboard FileBB[FILE_NB];
extern Bitboard RankBB[RANK_NB];
extern Bitboard SquareBB[SQUARE_NB];
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];

extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];

constexpr Bitboard square_bb(Square s) {
    return 1ULL << s;
}
constexpr Bitboard operator&(Bitboard b, Square s) {
    return b & square_bb(s);
}

constexpr Bitboard operator|(Bitboard b, Square s) {
    return b | square_bb(s);
}

constexpr Bitboard operator^(Bitboard b, Square s) {
    return b ^ square_bb(s);
}

inline Bitboard& operator|=(Bitboard& b, Square s) {
    return b |= square_bb(s);
}

inline Bitboard& operator^=(Bitboard& b, Square s) {
    return b ^= square_bb(s);
}

inline Bitboard& operator&=(Bitboard& b, Square s) {
    return b &= square_bb(s);
}

constexpr bool more_than_one(Bitboard b) {
    return b & (b - 1);
}

#if defined(__GNUC__) || defined(__clang__)
    inline int popcount(Bitboard b) {
        return __builtin_popcountll(b);
    }

    inline Square lsb(Bitboard b) {
        return Square(__builtin_ctzll(b));
    }

    inline Square msb(Bitboard b) {
        return Square(63 ^ __builtin_clzll(b));
    }

#elif defined(_MSC_VER)
    #include <intrin.h>

    inline int popcount(Bitboard b) {
        return static_cast<int>(__popcnt64(b));
    }

    inline Square lsb(Bitboard b) {
        unsigned long idx;
        _BitScanForward64(&idx, b);
        return Square(idx);
    }

    inline Square msb(Bitboard b) {
        unsigned long idx;
        _BitScanReverse64(&idx, b);
        return Square(idx);
    }

#else
    inline int popcount(Bitboard b) {
        int count = 0;
        while (b) {
            count++;
            b &= b - 1;
        }
        return count;
    }

    inline Square lsb(Bitboard b) {
        int idx = 0;
        while (!(b & 1)) {
            b >>= 1;
            idx++;
        }
        return Square(idx);
    }

    inline Square msb(Bitboard b) {
        int idx = 63;
        while (!(b & (1ULL << 63))) {
            b <<= 1;
            idx--;
        }
        return Square(idx);
    }
#endif

inline Square pop_lsb(Bitboard& b) {
    Square s = lsb(b);
    b &= b - 1;
    return s;
}

template<Direction D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH)      return b << 8;
    if constexpr (D == SOUTH)      return b >> 8;
    if constexpr (D == EAST)       return (b & NOT_FILE_H_BB) << 1;
    if constexpr (D == WEST)       return (b & NOT_FILE_A_BB) >> 1;
    if constexpr (D == NORTH_EAST) return (b & NOT_FILE_H_BB) << 9;
    if constexpr (D == NORTH_WEST) return (b & NOT_FILE_A_BB) << 7;
    if constexpr (D == SOUTH_EAST) return (b & NOT_FILE_H_BB) >> 7;
    if constexpr (D == SOUTH_WEST) return (b & NOT_FILE_A_BB) >> 9;
    return 0;
}

inline Bitboard pawn_attacks_bb(Color c, Square s) {
    return PawnAttacks[c][s];
}

template<Color C>
constexpr Bitboard pawn_attacks_bb(Bitboard b) {
    if constexpr (C == WHITE) {
        return shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b);
    } else {
        return shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
    }
}

inline Bitboard knight_attacks_bb(Square s) {
    return KnightAttacks[s];
}

inline Bitboard king_attacks_bb(Square s) {
    return KingAttacks[s];
}

inline Bitboard pawn_attacks_bb(Color c, Bitboard b) {
    if (c == WHITE) {
        return shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b);
    } else {
        return shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
    }
}

inline Bitboard between_bb(Square s1, Square s2) {
    return BetweenBB[s1][s2];
}

inline Bitboard line_bb(Square s1, Square s2) {
    return LineBB[s1][s2];
}

inline bool aligned(Square s1, Square s2, Square s3) {
    return LineBB[s1][s2] & s3;
}

namespace Bitboards {
    void init();
    std::string pretty(Bitboard b);
}

#endif
