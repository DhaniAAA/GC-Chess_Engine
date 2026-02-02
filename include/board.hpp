#ifndef BOARD_HPP
#define BOARD_HPP

#include "types.hpp"
#include "bitboard.hpp"
#include "magic.hpp"
#include "zobrist.hpp"
#include "move.hpp"
#include <string>
#include <vector>

struct StateInfo {
    Key positionKey;
    Key pawnKey;
    Key materialKey;
    EvalScore psqtScore[COLOR_NB];
    CastlingRights castling;
    Square enPassant;
    int halfmoveClock;
    int pliesFromNull;
    Piece capturedPiece;
    Bitboard checkers;
    Bitboard blockersForKing[COLOR_NB];
    Bitboard pinners[COLOR_NB];
    Bitboard checkSquares[PIECE_TYPE_NB];
    int repetition;
    StateInfo* previous;
};

class Board {
public:
    Board();
    Board(const std::string& fen);

    Board(const Board&) = default;
    Board& operator=(const Board&) = default;

    void set(const std::string& fen, StateInfo* si);
    std::string fen() const;

    static const std::string StartFEN;

    Piece piece_on(Square s) const { return board[s]; }
    bool empty(Square s) const { return piece_on(s) == NO_PIECE; }

    Bitboard pieces() const { return byTypeBB[0]; }
    Bitboard pieces(PieceType pt) const { return byTypeBB[pt]; }
    Bitboard pieces(PieceType pt1, PieceType pt2) const { return byTypeBB[pt1] | byTypeBB[pt2]; }
    Bitboard pieces(Color c) const { return byColorBB[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard pieces(Color c, PieceType pt1, PieceType pt2) const {
        return byColorBB[c] & (byTypeBB[pt1] | byTypeBB[pt2]);
    }

    Square king_square(Color c) const { return kingSquare[c]; }

    int count(Piece pc) const { return pieceCount[pc]; }
    int count(Color c, PieceType pt) const { return pieceCount[make_piece(c, pt)]; }

    Color side_to_move() const { return sideToMove; }
    CastlingRights castling_rights() const { return st->castling; }
    Square en_passant_square() const { return st->enPassant; }
    int halfmove_clock() const { return st->halfmoveClock; }
    int game_ply() const { return gamePly; }

    bool in_check() const { return st->checkers != EMPTY_BB; }
    Bitboard checkers() const { return st->checkers; }

    Bitboard blockers_for_king(Color c) const { return st->blockersForKing[c]; }
    Bitboard pinners(Color c) const { return st->pinners[c]; }

    Bitboard check_squares(PieceType pt) const { return st->checkSquares[pt]; }

    Key key() const { return st->positionKey; }
    Key pawn_key() const { return st->pawnKey; }
    Key material_key() const { return st->materialKey; }

    EvalScore psqt_score(Color c) const { return st->psqtScore[c]; }

    bool is_draw(int ply) const;
    bool has_repeated() const;

    Bitboard attackers_to(Square s) const;
    Bitboard attackers_to(Square s, Bitboard occupied) const;

    bool is_attacked_by(Color c, Square s) const;

    bool is_capture(Move m) const {
        return !empty(m.to()) || m.is_enpassant();
    }

    Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const;

    void do_move(Move m, StateInfo& newSt);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    bool is_valid() const;

    std::string pretty() const;

private:
    void clear();
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);

    void set_check_info();
    void set_state(StateInfo* si);

    Key compute_key() const;
    Key compute_pawn_key() const;
    Key compute_material_key() const;

    Piece board[SQUARE_NB];
    Bitboard byTypeBB[PIECE_TYPE_NB];
    Bitboard byColorBB[COLOR_NB];
    int pieceCount[PIECE_NB];
    Square kingSquare[COLOR_NB];

    static constexpr int MAX_PIECES_PER_TYPE = 10;
    Square pieceList[PIECE_NB][MAX_PIECES_PER_TYPE];
    int pieceIndex[SQUARE_NB];
    Color sideToMove;
    int gamePly;

    StateInfo* st;
    StateInfo startState;

    CastlingRights castlingRightsMask[SQUARE_NB];
};

namespace Position {
    void init();
}

#endif
