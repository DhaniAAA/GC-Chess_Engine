#ifndef BOARD_HPP
#define BOARD_HPP

#include "types.hpp"
#include "bitboard.hpp"
#include "magic.hpp"
#include "zobrist.hpp"
#include "move.hpp"
#include <string>
#include <vector>

// ============================================================================
// Board State
// Contains all information needed to represent a chess position
// ============================================================================

struct StateInfo {
    // Hash keys
    Key positionKey;       // Full position hash (Zobrist)
    Key pawnKey;           // Pawn structure hash (for pawn hash table)
    Key materialKey;       // Material configuration hash

    // Incremental material+PST score per color (updated in do_move/undo_move)
    EvalScore psqtScore[COLOR_NB];

    // Game state
    CastlingRights castling;
    Square enPassant;
    int halfmoveClock;     // 50-move rule counter
    int pliesFromNull;     // Ply count from last null move (for null move pruning)

    // Captured piece (for unmake)
    Piece capturedPiece;

    // Checkers bitboard
    Bitboard checkers;

    // Pinned pieces and blockers
    Bitboard blockersForKing[COLOR_NB];
    Bitboard pinners[COLOR_NB];

    // Check squares (squares from which each piece type gives check)
    Bitboard checkSquares[PIECE_TYPE_NB];

    // Repetition info
    int repetition;

    // Previous state (for unmake)
    StateInfo* previous;
};

// ============================================================================
// Board Class
// ============================================================================

class Board {
public:
    // ========================================================================
    // Constructors
    // ========================================================================

    Board();
    Board(const std::string& fen);

    Board(const Board&) = default;
    Board& operator=(const Board&) = default;

    // ========================================================================
    // FEN Parsing
    // ========================================================================

    void set(const std::string& fen, StateInfo* si);
    std::string fen() const;

    // Standard starting position
    static const std::string StartFEN;

    // ========================================================================
    // Board Access
    // ========================================================================

    // Get piece at a square
    Piece piece_on(Square s) const { return board[s]; }
    bool empty(Square s) const { return piece_on(s) == NO_PIECE; }

    // Get bitboard of pieces
    Bitboard pieces() const { return byTypeBB[0]; }  // All pieces
    Bitboard pieces(PieceType pt) const { return byTypeBB[pt]; }
    Bitboard pieces(PieceType pt1, PieceType pt2) const { return byTypeBB[pt1] | byTypeBB[pt2]; }
    Bitboard pieces(Color c) const { return byColorBB[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard pieces(Color c, PieceType pt1, PieceType pt2) const {
        return byColorBB[c] & (byTypeBB[pt1] | byTypeBB[pt2]);
    }

    // Get king square
    Square king_square(Color c) const { return kingSquare[c]; }

    // Piece count
    int count(Piece pc) const { return pieceCount[pc]; }
    int count(Color c, PieceType pt) const { return pieceCount[make_piece(c, pt)]; }

    // ========================================================================
    // Game State Access
    // ========================================================================

    Color side_to_move() const { return sideToMove; }
    CastlingRights castling_rights() const { return st->castling; }
    Square en_passant_square() const { return st->enPassant; }
    int halfmove_clock() const { return st->halfmoveClock; }
    int game_ply() const { return gamePly; }

    // King in check?
    bool in_check() const { return st->checkers != EMPTY_BB; }
    Bitboard checkers() const { return st->checkers; }

    // Blockers and pinners
    Bitboard blockers_for_king(Color c) const { return st->blockersForKing[c]; }
    Bitboard pinners(Color c) const { return st->pinners[c]; }

    // Check squares for a piece type
    Bitboard check_squares(PieceType pt) const { return st->checkSquares[pt]; }

    // Hash key
    Key key() const { return st->positionKey; }
    Key pawn_key() const { return st->pawnKey; }
    Key material_key() const { return st->materialKey; }

    // Incremental PST score (material + piece-square values)
    EvalScore psqt_score(Color c) const { return st->psqtScore[c]; }

    // Draw detection
    bool is_draw(int ply) const;       // Check for draw (repetition, 50-move, insufficient material)
    bool has_repeated() const;          // Check if current position has occurred before

    // ========================================================================
    // Attack Detection
    // ========================================================================

    // Get attackers to a square
    Bitboard attackers_to(Square s) const;
    Bitboard attackers_to(Square s, Bitboard occupied) const;

    // Is square attacked by color?
    bool is_attacked_by(Color c, Square s) const;

    // Slider blockers
    Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const;

    // ========================================================================
    // Move Making
    // ========================================================================

    void do_move(Move m, StateInfo& newSt);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // ========================================================================
    // Position Validation
    // ========================================================================

    bool is_valid() const;

    // ========================================================================
    // Debug / Display
    // ========================================================================

    std::string pretty() const;

private:
    // ========================================================================
    // Internal Methods
    // ========================================================================

    void clear();
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);

    void set_check_info();
    void set_state(StateInfo* si);

    Key compute_key() const;
    Key compute_pawn_key() const;
    Key compute_material_key() const;

    // ========================================================================
    // Board Data
    // ========================================================================

    // Piece placement
    Piece board[SQUARE_NB];          // Piece on each square
    Bitboard byTypeBB[PIECE_TYPE_NB]; // Bitboard for each piece type
    Bitboard byColorBB[COLOR_NB];     // Bitboard for each color
    int pieceCount[PIECE_NB];         // Count of each piece type
    Square kingSquare[COLOR_NB];      // King positions

    // ========================================================================
    // Piece-Lists (CPW Board Representation feature)
    // Enables O(1) iteration over specific piece types
    // ========================================================================
    static constexpr int MAX_PIECES_PER_TYPE = 10;  // Max pieces per type (for promotions)
    Square pieceList[PIECE_NB][MAX_PIECES_PER_TYPE]; // List of squares for each piece type
    int pieceIndex[SQUARE_NB];  // Index of piece in pieceList for O(1) removal

    // Game state
    Color sideToMove;
    int gamePly;

    // State stack
    StateInfo* st;
    StateInfo startState;  // Initial state storage

    // Castling masks (which squares affect castling rights)
    CastlingRights castlingRightsMask[SQUARE_NB];
};

// ============================================================================
// Global Initialization
// ============================================================================

namespace Position {
    void init();  // Initialize all subsystems (bitboards, magic, zobrist)
}

#endif // BOARD_HPP
