#ifndef TYPES_HPP
#define TYPES_HPP

#include <cstdint>
#include <string>

using U64 = uint64_t;
using U32 = uint32_t;
using U16 = uint16_t;
using U8  = uint8_t;

using S64 = int64_t;
using S32 = int32_t;
using S16 = int16_t;
using S8  = int8_t;

enum Color : int {
    WHITE = 0,
    BLACK = 1,
    COLOR_NB = 2
};

enum PieceType : int {
    NO_PIECE_TYPE = 0,
    PAWN = 1,
    KNIGHT = 2,
    BISHOP = 3,
    ROOK = 4,
    QUEEN = 5,
    KING = 6,
    PIECE_TYPE_NB = 7
};

enum Piece : int {
    NO_PIECE = 0,
    W_PAWN = 1, W_KNIGHT = 2, W_BISHOP = 3, W_ROOK = 4, W_QUEEN = 5, W_KING = 6,
    B_PAWN = 9, B_KNIGHT = 10, B_BISHOP = 11, B_ROOK = 12, B_QUEEN = 13, B_KING = 14,
    PIECE_NB = 16
};

enum Square : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE,
    SQUARE_NB = 64
};

enum File : int {
    FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
    FILE_NB = 8
};

enum Rank : int {
    RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8,
    RANK_NB = 8
};

enum Direction : int {
    NORTH = 8,
    EAST = 1,
    SOUTH = -8,
    WEST = -1,

    NORTH_EAST = NORTH + EAST,
    NORTH_WEST = NORTH + WEST,
    SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST
};

enum CastlingRights : int {
    NO_CASTLING = 0,
    WHITE_OO = 1,
    WHITE_OOO = 2,
    BLACK_OO = 4,
    BLACK_OOO = 8,

    WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ALL_CASTLING = WHITE_CASTLING | BLACK_CASTLING,

    CASTLING_RIGHT_NB = 16
};

constexpr Color operator~(Color c) {
    return Color(c ^ BLACK);
}

constexpr Square operator+(Square s, Direction d) {
    return Square(int(s) + int(d));
}

constexpr Square operator-(Square s, Direction d) {
    return Square(int(s) - int(d));
}

inline Square& operator+=(Square& s, Direction d) {
    return s = s + d;
}

inline Square& operator-=(Square& s, Direction d) {
    return s = s - d;
}

constexpr Square& operator++(Square& s) {
    return s = Square(int(s) + 1);
}

constexpr Square& operator--(Square& s) {
    return s = Square(int(s) - 1);
}

constexpr File& operator++(File& f) {
    return f = File(int(f) + 1);
}

constexpr Rank& operator++(Rank& r) {
    return r = Rank(int(r) + 1);
}

constexpr Rank& operator--(Rank& r) {
    return r = Rank(int(r) - 1);
}

constexpr Piece& operator++(Piece& pc) {
    return pc = Piece(int(pc) + 1);
}

constexpr PieceType& operator++(PieceType& pt) {
    return pt = PieceType(int(pt) + 1);
}

constexpr CastlingRights operator|(CastlingRights cr1, CastlingRights cr2) {
    return CastlingRights(int(cr1) | int(cr2));
}

constexpr CastlingRights operator&(CastlingRights cr1, CastlingRights cr2) {
    return CastlingRights(int(cr1) & int(cr2));
}

inline CastlingRights& operator|=(CastlingRights& cr1, CastlingRights cr2) {
    return cr1 = cr1 | cr2;
}

inline CastlingRights& operator&=(CastlingRights& cr1, CastlingRights cr2) {
    return cr1 = cr1 & cr2;
}

constexpr CastlingRights operator~(CastlingRights cr) {
    return CastlingRights(~int(cr));
}

constexpr Square make_square(File f, Rank r) {
    return Square((r << 3) + f);
}

constexpr File file_of(Square s) {
    return File(s & 7);
}

constexpr Rank rank_of(Square s) {
    return Rank(s >> 3);
}

constexpr Rank relative_rank(Color c, Rank r) {
    return Rank(r ^ (c * 7));
}

constexpr Rank relative_rank(Color c, Square s) {
    return relative_rank(c, rank_of(s));
}

constexpr Square relative_square(Color c, Square s) {
    return Square(s ^ (c * 56));
}

constexpr Direction pawn_push(Color c) {
    return c == WHITE ? NORTH : SOUTH;
}

constexpr Piece make_piece(Color c, PieceType pt) {
    return Piece((c << 3) + pt);
}

constexpr Color color_of(Piece pc) {
    return Color(pc >> 3);
}

constexpr PieceType type_of(Piece pc) {
    return PieceType(pc & 7);
}

constexpr bool is_valid_square(Square s) {
    return s >= SQ_A1 && s <= SQ_H8;
}

inline std::string square_to_string(Square s) {
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

inline Square string_to_square(const std::string& str) {
    if (str.length() < 2) return SQ_NONE;
    File f = File(str[0] - 'a');
    Rank r = Rank(str[1] - '1');
    if (f < FILE_A || f > FILE_H || r < RANK_1 || r > RANK_8) return SQ_NONE;
    return make_square(f, r);
}

struct EvalScore {
    int mg;
    int eg;

    constexpr EvalScore() : mg(0), eg(0) {}
    constexpr EvalScore(int m, int e) : mg(m), eg(e) {}

    constexpr EvalScore operator+(const EvalScore& other) const {
        return EvalScore(mg + other.mg, eg + other.eg);
    }
    constexpr EvalScore operator-(const EvalScore& other) const {
        return EvalScore(mg - other.mg, eg - other.eg);
    }
    constexpr EvalScore operator-() const {
        return EvalScore(-mg, -eg);
    }
    EvalScore& operator+=(const EvalScore& other) {
        mg += other.mg;
        eg += other.eg;
        return *this;
    }
    EvalScore& operator-=(const EvalScore& other) {
        mg -= other.mg;
        eg -= other.eg;
        return *this;
    }
    constexpr EvalScore operator*(int n) const {
        return EvalScore(mg * n, eg * n);
    }
};

constexpr EvalScore S(int mg, int eg) { return EvalScore(mg, eg); }

#endif
