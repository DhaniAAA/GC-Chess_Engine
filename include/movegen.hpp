#ifndef MOVEGEN_HPP
#define MOVEGEN_HPP

#include "board.hpp"
#include "move.hpp"
#include "magic.hpp"

// ============================================================================
// Move Generation Types
// ============================================================================

enum GenType {
    CAPTURES,      // Only capture moves
    QUIETS,        // Only quiet (non-capture) moves
    QUIET_CHECKS,  // Quiet moves that give check
    EVASIONS,      // Moves when in check
    NON_EVASIONS,  // All moves when not in check
    LEGAL          // Only legal moves
};

// ============================================================================
// Move Generator Class
// ============================================================================

class MoveGen {
public:
    // Generate all pseudo-legal moves
    static void generate_all(const Board& board, MoveList& moves);

    // Generate specific types of moves
    static void generate_captures(const Board& board, MoveList& moves);
    static void generate_quiets(const Board& board, MoveList& moves);
    static void generate_evasions(const Board& board, MoveList& moves);

    // Generate quiet moves that give check (optimized for QSearch)
    static void generate_checking_moves(const Board& board, MoveList& moves);

    // Generate legal moves only
    static void generate_legal(const Board& board, MoveList& moves);

    // Check if a move is pseudo-legal
    static bool is_pseudo_legal(const Board& board, Move m);

    // Check if a pseudo-legal move is legal
    static bool is_legal(const Board& board, Move m);

    // Check if a move gives check
    static bool gives_check(const Board& board, Move m);

private:
    // Internal generation helpers
    template<Color Us>
    static void generate_pawn_moves(const Board& board, MoveList& moves, Bitboard target);

    template<Color Us>
    static void generate_pawn_captures(const Board& board, MoveList& moves, Bitboard target);

    template<PieceType Pt>
    static void generate_piece_moves(const Board& board, MoveList& moves,
                                     Color us, Bitboard target);

    template<Color Us>
    static void generate_castling(const Board& board, MoveList& moves);

    // Helper to add pawn promotions
    static void add_pawn_moves(MoveList& moves, Square from, Square to, bool is_capture);
    static void add_promotions(MoveList& moves, Square from, Square to);
};

#endif // MOVEGEN_HPP
