#ifndef MOVE_HPP
#define MOVE_HPP

#include "types.hpp"

enum MoveType : int {
    NORMAL = 0,
    PROMOTION = 1 << 14,
    EN_PASSANT = 2 << 14,
    CASTLING = 3 << 14
};

class Move {
public:
    constexpr Move() : data(0) {}
    constexpr explicit Move(U16 d) : data(d) {}

    static constexpr Move make(Square from, Square to) {
        return Move(U16(to) | (U16(from) << 6));
    }

    static constexpr Move make_promotion(Square from, Square to, PieceType pt) {
        return Move(U16(to) | (U16(from) << 6) | PROMOTION | ((pt - KNIGHT) << 12));
    }
    static constexpr Move make_enpassant(Square from, Square to) {
        return Move(U16(to) | (U16(from) << 6) | EN_PASSANT);
    }

    static constexpr Move make_castling(Square from, Square to) {
        return Move(U16(to) | (U16(from) << 6) | CASTLING);
    }
    constexpr Square from() const { return Square((data >> 6) & 0x3F); }
    constexpr Square to() const { return Square(data & 0x3F); }
    constexpr MoveType type() const { return MoveType(data & (3 << 14)); }
    constexpr PieceType promotion_type() const { return PieceType(((data >> 12) & 3) + KNIGHT); }

    constexpr bool is_promotion() const { return type() == PROMOTION; }
    constexpr bool is_enpassant() const { return type() == EN_PASSANT; }
    constexpr bool is_castling() const { return type() == CASTLING; }
    constexpr bool is_normal() const { return type() == NORMAL; }

    constexpr bool is_ok() const { return data != 0 && from() != to(); }
    constexpr bool is_none() const { return data == 0; }

    constexpr U16 raw() const { return data; }

    constexpr bool operator==(Move m) const { return data == m.data; }
    constexpr bool operator!=(Move m) const { return data != m.data; }

    constexpr operator bool() const { return data != 0; }

private:
    U16 data;
};

constexpr Move MOVE_NONE = Move();
constexpr Move MOVE_NULL = Move(65);

inline std::string move_to_string(Move m) {
    if (m.is_none()) return "0000";

    std::string s = square_to_string(m.from()) + square_to_string(m.to());

    if (m.is_promotion()) {
        Rank fromRank = rank_of(m.from());
        Rank toRank = rank_of(m.to());
        bool isValidPromotion = (fromRank == RANK_7 && toRank == RANK_8) ||
                                (fromRank == RANK_2 && toRank == RANK_1);
        if (isValidPromotion) {
            s += "nbrq"[m.promotion_type() - KNIGHT];
        }
    }

    return s;
}

inline Move string_to_move(const std::string& str) {
    if (str.length() < 4) return MOVE_NONE;

    Square from = string_to_square(str.substr(0, 2));
    Square to = string_to_square(str.substr(2, 2));

    if (from == SQ_NONE || to == SQ_NONE) return MOVE_NONE;

    if (str.length() >= 5) {
        PieceType pt = NO_PIECE_TYPE;
        switch (str[4]) {
            case 'n': case 'N': pt = KNIGHT; break;
            case 'b': case 'B': pt = BISHOP; break;
            case 'r': case 'R': pt = ROOK; break;
            case 'q': case 'Q': pt = QUEEN; break;
        }
        if (pt != NO_PIECE_TYPE) {
            return Move::make_promotion(from, to, pt);
        }
    }

    return Move::make(from, to);
}

struct ScoredMove {
    Move move;
    int32_t score;

    constexpr ScoredMove() : move(MOVE_NONE), score(0) {}
    constexpr ScoredMove(Move m, int32_t s) : move(m), score(s) {}

    constexpr bool operator<(const ScoredMove& other) const {
        return score > other.score;
    }
};

class MoveList {
public:
    static constexpr int MAX_MOVES = 256;

    MoveList() : count(0) {}

    void add(Move m) {
        if (count < MAX_MOVES) {
            moves[count++] = ScoredMove(m, 0);
        }
    }

    void add(Move m, int32_t score) {
        if (count < MAX_MOVES) {
            moves[count++] = ScoredMove(m, score);
        }
    }

    int size() const { return count; }
    bool empty() const { return count == 0; }
    void clear() { count = 0; }
    void resize(int n) { if (n >= 0 && n <= count) count = n; }

    ScoredMove& operator[](int i) { return moves[i]; }
    const ScoredMove& operator[](int i) const { return moves[i]; }

    ScoredMove* begin() { return moves; }
    ScoredMove* end() { return moves + count; }
    const ScoredMove* begin() const { return moves; }
    const ScoredMove* end() const { return moves + count; }

    Move pick_best(int start) {
        int best_idx = start;
        for (int i = start + 1; i < count; ++i) {
            if (moves[i].score > moves[best_idx].score) {
                best_idx = i;
            }
        }
        if (best_idx != start) {
            std::swap(moves[start], moves[best_idx]);
        }
        return moves[start].move;
    }

private:
    ScoredMove moves[MAX_MOVES];
    int count;
};

#endif
