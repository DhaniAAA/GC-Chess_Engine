#ifndef BOOK_HPP
#define BOOK_HPP

#include "board.hpp"
#include "move.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <algorithm>

namespace Book {

extern const U64* PolyglotRandomPiece;
extern const U64* PolyglotRandomCastling;
extern const U64* PolyglotRandomEnPassant;
extern const U64 PolyglotRandomTurn;

inline int polyglot_piece(Piece pc) {
    if (pc == NO_PIECE) return -1;

    PieceType pt = type_of(pc);
    Color c = color_of(pc);

    int base = 0;
    switch (pt) {
        case PAWN:   base = 0; break;
        case KNIGHT: base = 1; break;
        case BISHOP: base = 2; break;
        case ROOK:   base = 3; break;
        case QUEEN:  base = 4; break;
        case KING:   base = 5; break;
        default:     return -1;
    }

    return base * 2 + (c == WHITE ? 1 : 0);
}

inline U64 polyglot_key(const Board& board) {
    U64 key = 0;

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece pc = board.piece_on(s);
        if (pc != NO_PIECE) {
            int poly_piece = polyglot_piece(pc);
            if (poly_piece >= 0) {
                key ^= PolyglotRandomPiece[64 * poly_piece + s];
            }
        }
    }

    int castling = 0;
    CastlingRights cr = board.castling_rights();
    if (cr & WHITE_OO)  castling |= 1;
    if (cr & WHITE_OOO) castling |= 2;
    if (cr & BLACK_OO)  castling |= 4;
    if (cr & BLACK_OOO) castling |= 8;
    key ^= PolyglotRandomCastling[castling];

    Square ep = board.en_passant_square();
    if (ep != SQ_NONE) {
        File f = file_of(ep);
        Bitboard pawns = board.pieces(board.side_to_move(), PAWN);
        Bitboard canCapture = 0;
        int offset = (board.side_to_move() == WHITE) ? -8 : 8;
        if (f > FILE_A) canCapture |= square_bb(Square(int(ep) - 1 + offset));
        if (f < FILE_H) canCapture |= square_bb(Square(int(ep) + 1 + offset));

        if (pawns & canCapture) {
            key ^= PolyglotRandomEnPassant[f];
        }
    }

    if (board.side_to_move() == WHITE) {
        key ^= PolyglotRandomTurn;
    }

    return key;
}

inline Move decode_polyglot_move(const Board& board, uint16_t poly_move) {
    int to_sq = poly_move & 0x3F;
    int from_sq = (poly_move >> 6) & 0x3F;
    int promo = (poly_move >> 12) & 0x7;

    Square from = Square(from_sq);
    Square to = Square(to_sq);

    Piece pc = board.piece_on(from);
    if (type_of(pc) == KING) {
        if (from == SQ_E1 && to == SQ_H1) to = SQ_G1;
        if (from == SQ_E8 && to == SQ_H8) to = SQ_G8;
        if (from == SQ_E1 && to == SQ_A1) to = SQ_C1;
        if (from == SQ_E8 && to == SQ_A8) to = SQ_C8;
    }
    if (promo > 0) {
        PieceType promo_pt = PieceType(promo + 1);
        return Move::make_promotion(from, to, promo_pt);
    }

    if (type_of(pc) == PAWN && to == board.en_passant_square()) {
        return Move::make_enpassant(from, to);
    }

    if (type_of(pc) == KING && std::abs(file_of(from) - file_of(to)) > 1) {
        return Move::make_castling(from, to);
    }

    return Move::make(from, to);
}

struct BookEntry {
    U64 key;
    uint16_t move;
    uint16_t weight;
    uint32_t learn;

    BookEntry() : key(0), move(0), weight(0), learn(0) {}
};

class OpeningBook {
public:
    OpeningBook() : loaded(false), variety(true) {}

    bool load(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        entries.clear();

        while (file) {
            BookEntry entry;

            U64 key = 0;
            for (int i = 0; i < 8; ++i) {
                char c;
                if (!file.get(c)) break;
                key = (key << 8) | static_cast<unsigned char>(c);
            }
            if (!file) break;
            entry.key = key;

            char buf[2];
            file.read(buf, 2);
            if (!file) break;
            entry.move = (static_cast<unsigned char>(buf[0]) << 8) |
                          static_cast<unsigned char>(buf[1]);

            file.read(buf, 2);
            if (!file) break;
            entry.weight = (static_cast<unsigned char>(buf[0]) << 8) |
                            static_cast<unsigned char>(buf[1]);
            char learn_buf[4];
            file.read(learn_buf, 4);
            if (!file) break;

            entries.push_back(entry);
        }

        std::sort(entries.begin(), entries.end(),
            [](const BookEntry& a, const BookEntry& b) {
                return a.key < b.key;
            });

        loaded = !entries.empty();
        return loaded;
    }

    bool is_loaded() const { return loaded; }

    size_t size() const { return entries.size(); }
    void set_variety(bool v) { variety = v; }

    Move probe(const Board& board) const {
        if (!loaded) return MOVE_NONE;

        U64 key = polyglot_key(board);
        auto lower = std::lower_bound(entries.begin(), entries.end(), key,
            [](const BookEntry& e, U64 k) { return e.key < k; });

        if (lower == entries.end() || lower->key != key) {
            return MOVE_NONE;
        }

        std::vector<std::pair<Move, int>> moves;
        for (auto it = lower; it != entries.end() && it->key == key; ++it) {
            Move m = decode_polyglot_move(board, it->move);
            if (m != MOVE_NONE) {
                moves.push_back({m, it->weight});
            }
        }

        if (moves.empty()) {
            return MOVE_NONE;
        }

        if (variety && moves.size() > 1) {
            int totalWeight = 0;
            for (const auto& mv : moves) {
                totalWeight += mv.second;
            }

            static std::random_device rd;
            static std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, totalWeight - 1);

            int pick = dis(gen);
            int cumulative = 0;
            for (const auto& mv : moves) {
                cumulative += mv.second;
                if (pick < cumulative) {
                    return mv.first;
                }
            }
        }

        auto best = std::max_element(moves.begin(), moves.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        return best->first;
    }

    std::vector<std::pair<Move, int>> get_moves(const Board& board) const {
        std::vector<std::pair<Move, int>> moves;
        if (!loaded) return moves;

        U64 key = polyglot_key(board);

        auto lower = std::lower_bound(entries.begin(), entries.end(), key,
            [](const BookEntry& e, U64 k) { return e.key < k; });

        for (auto it = lower; it != entries.end() && it->key == key; ++it) {
            Move m = decode_polyglot_move(board, it->move);
            if (m != MOVE_NONE) {
                moves.push_back({m, it->weight});
            }
        }

        std::sort(moves.begin(), moves.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        return moves;
    }

private:
    std::vector<BookEntry> entries;
    bool loaded;
    bool variety;
};

extern OpeningBook book;

}

#endif
