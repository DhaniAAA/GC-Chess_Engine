#ifndef ZOBRIST_HPP
#define ZOBRIST_HPP

#include "types.hpp"
#include "bitboard.hpp"

using Key = U64;

namespace Zobrist {
extern Key PieceSquare[PIECE_NB][SQUARE_NB];
extern Key Castling[CASTLING_RIGHT_NB];
extern Key EnPassant[FILE_NB];
extern Key SideToMove;
}

namespace Zobrist {
extern Key PieceSquare[PIECE_NB][SQUARE_NB];
extern Key Castling[CASTLING_RIGHT_NB];
extern Key EnPassant[FILE_NB];
extern Key SideToMove;

void init();

inline Key piece_key(Piece pc, Square sq) {
    return PieceSquare[pc][sq];
}

inline Key castling_key(CastlingRights cr) {
    return Castling[cr];
}

inline Key enpassant_key(File f) {
    return EnPassant[f];
}

inline Key side_key() {
    return SideToMove;
}

}

#endif
