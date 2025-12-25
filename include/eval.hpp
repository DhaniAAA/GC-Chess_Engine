#ifndef EVAL_HPP
#define EVAL_HPP

#include "board.hpp"
#include "magic.hpp"
#include <algorithm>
#include "tuning.hpp"

namespace Eval {

// ============================================================================
// Material Values (from Tuning)
// ============================================================================

using Tuning::PawnValue;
using Tuning::KnightValue;
using Tuning::BishopValue;
using Tuning::RookValue;
using Tuning::QueenValue;

// ============================================================================
// Game Phase Constants
// ============================================================================

constexpr int PhaseValue[PIECE_TYPE_NB] = { 0, 0, 1, 1, 2, 4, 0 };
constexpr int TotalPhase = 24;  // 4*1 (knights) + 4*1 (bishops) + 4*2 (rooks) + 2*4 (queens)

// ============================================================================
// Piece-Square Tables (Middlegame and Endgame)
// ============================================================================

// Pawn PST (from white's perspective)
constexpr EvalScore PawnPST[SQUARE_NB] = {
    S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0),
    S( -1,  0), S(  4,  0), S( -8,  0), S(-24,  0), S(-24,  0), S( -8,  0), S(  4,  0), S( -1,  0),
    S( -2,  0), S(  2,  0), S(  0,  0), S( -4,  0), S( -4,  0), S(  0,  0), S(  2,  0), S( -2,  0),
    S(  0,  5), S(  0,  5), S(  4, 10), S( 18, 15), S( 18, 15), S(  4, 10), S(  0,  5), S(  0,  5),
    S(  5, 10), S( 10, 15), S( 15, 20), S( 25, 25), S( 25, 25), S( 15, 20), S( 10, 15), S(  5, 10),
    S( 10, 25), S( 20, 35), S( 30, 45), S( 40, 55), S( 40, 55), S( 30, 45), S( 20, 35), S( 10, 25),
    S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70), S( 50, 70),
    S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0)
};

// Knight PST
constexpr EvalScore KnightPST[SQUARE_NB] = {
    S(-50,-50), S(-40,-40), S(-30,-30), S(-30,-30), S(-30,-30), S(-30,-30), S(-40,-40), S(-50,-50),
    S(-40,-40), S(-20,-20), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(-20,-20), S(-40,-40),
    S(-30,-30), S(  5,  0), S( 15, 10), S( 20, 15), S( 20, 15), S( 15, 10), S(  5,  0), S(-30,-30),
    S(-30,-25), S(  5,  5), S( 20, 15), S( 25, 25), S( 25, 25), S( 20, 15), S(  5,  5), S(-30,-25),
    S(-30,-25), S( 10,  5), S( 20, 15), S( 25, 25), S( 25, 25), S( 20, 15), S( 10,  5), S(-30,-25),
    S(-30,-30), S(  5,  0), S( 15, 10), S( 20, 15), S( 20, 15), S( 15, 10), S(  5,  0), S(-30,-30),
    S(-40,-40), S(-20,-20), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(-20,-20), S(-40,-40),
    S(-50,-50), S(-40,-40), S(-30,-30), S(-30,-30), S(-30,-30), S(-30,-30), S(-40,-40), S(-50,-50)
};

// Bishop PST
constexpr EvalScore BishopPST[SQUARE_NB] = {
    S(-20,-20), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-20,-20),
    S(-10,-10), S( 10,  5), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S( 10,  5), S(-10,-10),
    S(-10,-10), S( 10,  5), S( 10, 10), S( 10, 10), S( 10, 10), S( 10, 10), S( 10,  5), S(-10,-10),
    S(-10,-10), S(  0,  5), S( 10, 10), S( 15, 15), S( 15, 15), S( 10, 10), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  5,  5), S( 10, 10), S( 15, 15), S( 15, 15), S( 10, 10), S(  5,  5), S(-10,-10),
    S(-10,-10), S(  0,  5), S( 10, 10), S( 10, 10), S( 10, 10), S( 10, 10), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  5,  5), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(  5,  5), S(-10,-10),
    S(-20,-20), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-10,-10), S(-20,-20)
};

// Rook PST
constexpr EvalScore RookPST[SQUARE_NB] = {
    S(  0, -5), S(  0, -5), S(  5,  0), S( 10,  5), S( 10,  5), S(  5,  0), S(  0, -5), S(  0, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  0,  0), S(  0,  5), S(  0,  5), S(  0,  0), S(  0,  0), S( -5, -5),
    S( 10,  5), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 15, 10), S( 10,  5),
    S(  0,  0), S(  0,  0), S(  0,  0), S(  5,  5), S(  5,  5), S(  0,  0), S(  0,  0), S(  0,  0)
};

// Queen PST
constexpr EvalScore QueenPST[SQUARE_NB] = {
    S(-20,-20), S(-10,-10), S(-10,-10), S( -5,-10), S( -5,-10), S(-10,-10), S(-10,-10), S(-20,-20),
    S(-10,-10), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(-10,-10),
    S(-10,-10), S(  0,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  0,  5), S(-10,-10),
    S( -5, -5), S(  0,  0), S(  5,  5), S(  5, 10), S(  5, 10), S(  5,  5), S(  0,  0), S( -5, -5),
    S( -5, -5), S(  0,  0), S(  5,  5), S(  5, 10), S(  5, 10), S(  5,  5), S(  0,  0), S( -5, -5),
    S(-10,-10), S(  0,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  5,  5), S(  0,  5), S(-10,-10),
    S(-10,-10), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(  0,  0), S(-10,-10),
    S(-20,-20), S(-10,-10), S(-10,-10), S( -5,-10), S( -5,-10), S(-10,-10), S(-10,-10), S(-20,-20)
};

// King PST Middlegame (prefer castled position)
constexpr int KingPSTMG[SQUARE_NB] = {
     20,  30,  10,   0,   0,  10,  30,  20,
     20,  20,   0, -10, -10,   0,  20,  20,
    -10, -20, -20, -30, -30, -20, -20, -10,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40,
    -40, -50, -50, -60, -60, -50, -50, -40
};

// King PST Endgame (prefer centralized king)
constexpr int KingPSTEG[SQUARE_NB] = {
    -50, -30, -20, -20, -20, -20, -30, -50,
    -30, -10,   0,  10,  10,   0, -10, -30,
    -20,   0,  15,  25,  25,  15,   0, -20,
    -20,  10,  25,  35,  35,  25,  10, -20,
    -20,  10,  25,  35,  35,  25,  10, -20,
    -20,   0,  15,  25,  25,  15,   0, -20,
    -30, -10,   0,  10,  10,   0, -10, -30,
    -50, -30, -20, -20, -20, -20, -30, -50
};

// ============================================================================
// Evaluation Bonuses and Penalties
// ============================================================================

// Pawn structure
constexpr EvalScore PassedPawnBonus[8] = {
    S(  0,   0), S(  5,  10), S( 10,  20), S( 20,  40),
    S( 40,  75), S( 70, 120), S(100, 180), S(  0,   0)
};

constexpr EvalScore ConnectedPassedBonus[8] = {
    S(  0,   0), S(  5,   8), S( 10,  15), S( 15,  25),
    S( 25,  45), S( 40,  70), S( 60, 100), S(  0,   0)
};

using Tuning::IsolatedPawnPenalty;
using Tuning::DoubledPawnPenalty;
using Tuning::BackwardPawnPenalty;
using Tuning::ConnectedPawnBonus;
using Tuning::PhalanxBonus;

// King safety - semi-open file near enemy king
constexpr EvalScore KingSemiOpenFilePenalty = S( 15, 0);
constexpr EvalScore KingOpenFilePenalty     = S( 25, 0);

// Piece activity
using Tuning::BishopPairBonus;
using Tuning::RookOpenFileBonus;
using Tuning::RookSemiOpenFileBonus;
using Tuning::RookOnSeventhBonus;
using Tuning::KnightOutpostBonus;

// Mobility bonus per square
constexpr EvalScore KnightMobility[9] = {
    S(-30, -40), S(-15, -20), S( -5, -10), S(  0,  0),
    S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
    S( 20,  20)
};
constexpr EvalScore BishopMobility[14] = {
    S(-25, -35), S(-15, -20), S( -5, -10), S(  0,  0),
    S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
    S( 20,  20), S( 22,  22), S( 24,  24), S( 25, 25),
    S( 26,  26), S( 27,  27)
};
constexpr EvalScore RookMobility[15] = {
    S(-20, -30), S(-12, -18), S( -5, -10), S(  0,  0),
    S(  5,   5), S(  8,  10), S( 10,  15), S( 12, 18),
    S( 14,  20), S( 16,  22), S( 17,  24), S( 18, 25),
    S( 19,  26), S( 20,  27), S( 20,  28)
};
constexpr EvalScore QueenMobility[28] = {
    S(-15, -25), S(-10, -15), S( -5, -10), S(  0,  0),
    S(  2,   3), S(  4,   5), S(  5,   7), S(  6,  8),
    S(  7,   9), S(  8,  10), S(  9,  11), S( 10, 12),
    S( 10,  13), S( 11,  13), S( 11,  14), S( 12, 14),
    S( 12,  15), S( 13,  15), S( 13,  16), S( 14, 16),
    S( 14,  17), S( 15,  17), S( 15,  18), S( 15, 18),
    S( 16,  18), S( 16,  19), S( 16,  19), S( 17, 20)
};

// King safety (attack unit weights)
constexpr int KnightAttackWeight = 2;
constexpr int BishopAttackWeight = 2;
constexpr int RookAttackWeight   = 3;
constexpr int QueenAttackWeight  = 5;

// King safety lookup table
constexpr int KingSafetyTable[100] = {
    0,   0,   1,   2,   3,   5,   7,  10,  13,  16,
   20,  25,  30,  36,  42,  49,  56,  64,  72,  81,
   90, 100, 110, 121, 132, 144, 156, 169, 182, 196,
  210, 225, 240, 256, 272, 289, 306, 324, 342, 361,
  380, 400, 420, 441, 462, 484, 506, 529, 552, 576,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600,
  600, 600, 600, 600, 600, 600, 600, 600, 600, 600
};

// Pawn shield bonus (MG only)
constexpr int PawnShieldBonus[4] = { 0, 10, 20, 30 };

// ============================================================================
// Function Declarations
// ============================================================================

// Helper Functions
Square flip_square(Square sq);
Bitboard file_bb(File f);
Bitboard adjacent_files_bb(File f);
Bitboard rank_bb_eval(Rank r);
Bitboard pawn_front_span(Color c, Square s);
Bitboard passed_pawn_mask(Color c, Square s);
Square pawn_push(Color c, Square s);
bool is_backward_pawn(Color c, Square s, Bitboard ourPawns, Bitboard theirPawns);

// Evaluation Components
EvalScore eval_material_pst(const Board& board, Color c);
EvalScore eval_pawn_structure(const Board& board, Color c);
EvalScore eval_pieces(const Board& board, Color c);
EvalScore eval_king_safety(const Board& board, Color c);
EvalScore eval_space(const Board& board, Color c);

// Incremental PST helper - returns material + PST score for a single piece
// Used by Board::do_move/undo_move for incremental updates
inline EvalScore piece_pst_score(Piece pc, Square sq) {
    Color c = color_of(pc);
    PieceType pt = type_of(pc);
    Square psq = c == WHITE ? sq : Square(sq ^ 56);  // flip for black

    switch (pt) {
        case PAWN:   return PawnValue + PawnPST[psq];
        case KNIGHT: return KnightValue + KnightPST[psq];
        case BISHOP: return BishopValue + BishopPST[psq];
        case ROOK:   return RookValue + RookPST[psq];
        case QUEEN:  return QueenValue + QueenPST[psq];
        case KING:   return EvalScore(KingPSTMG[psq], KingPSTEG[psq]);
        default:     return EvalScore(0, 0);
    }
}

// ============================================================================
// Pawn Hash Table
// ============================================================================

struct PawnEntry {
    Key key;
    EvalScore score;

    // Check if the entry matches the key
    bool match(Key k) const { return key == k; }
};

class PawnTable {
public:
    static constexpr int SIZE = 16384;  // 16K entries, adjustable
    static constexpr int MASK = SIZE - 1;

    void clear() {
        for (int i = 0; i < SIZE; ++i) {
            table[i].key = 0;
            table[i].score = S(0, 0);
        }
    }

    PawnEntry* probe(Key key) {
        return &table[key & MASK];
    }

private:
    PawnEntry table[SIZE];
};

extern PawnTable pawnTable;

// Main Evaluation Functions
// Version with alpha/beta enables lazy evaluation (skips expensive calcs if far from window)
int evaluate(const Board& board, int alpha, int beta);
int evaluate(const Board& board);  // Compatibility version (no lazy eval)

// Thread-safe version without pawn hash table (for Texel Tuner multi-threading)
int evaluate_no_cache(const Board& board);

// Material balance for contempt (positive = white ahead, negative = black ahead)
int material_balance(const Board& board);

} // namespace Eval

#endif // EVAL_HPP
