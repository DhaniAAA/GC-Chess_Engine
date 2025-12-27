#ifndef EVAL_HPP
#define EVAL_HPP

#include "board.hpp"
#include "magic.hpp"
#include <algorithm>
#include <cstring>
#include "tuning.hpp"

namespace Eval {

// ============================================================================
// EvalContext (CPW Attack and Defend Maps)
// Caches attack information to avoid redundant calculations during evaluation.
// ============================================================================

constexpr int ATTACK_PIECE_TYPES = PIECE_TYPE_NB + 1;
constexpr int ALL_PIECES = PIECE_TYPE_NB;

struct EvalContext {
    Bitboard attackedBy[COLOR_NB][ATTACK_PIECE_TYPES];
    Bitboard attackedBy2[COLOR_NB];  // Double attacks
    Bitboard kingRing[COLOR_NB];
    Bitboard innerKingRing[COLOR_NB];  // Immediately adjacent squares (8 max)
    Bitboard outerKingRing[COLOR_NB];  // Two squares away from king (16 max)
    Square kingSquare[COLOR_NB];
    int kingAttackersCount[COLOR_NB];
    int kingAttackersWeight[COLOR_NB];
    int innerRingAttacks[COLOR_NB];  // Attacks on inner ring
    int outerRingAttacks[COLOR_NB];  // Attacks on outer ring
    Bitboard mobilityArea[COLOR_NB];
    bool initialized;

    EvalContext() : initialized(false) { clear(); }

    void clear() {
        for (int c = 0; c < COLOR_NB; ++c) {
            for (int pt = 0; pt < ATTACK_PIECE_TYPES; ++pt) {
                attackedBy[c][pt] = 0;
            }
            attackedBy2[c] = 0;
            kingRing[c] = 0;
            innerKingRing[c] = 0;
            outerKingRing[c] = 0;
            kingSquare[c] = SQ_NONE;
            kingAttackersCount[c] = 0;
            kingAttackersWeight[c] = 0;
            innerRingAttacks[c] = 0;
            outerRingAttacks[c] = 0;
            mobilityArea[c] = 0;
        }
        initialized = false;
    }
};

void init_eval_context(EvalContext& ctx, const Board& board);
EvalScore eval_pieces_with_context(const Board& board, Color c, EvalContext& ctx);
EvalScore eval_king_safety_with_context(const Board& board, Color c, EvalContext& ctx);
EvalScore eval_threats_with_context(const Board& board, Color c, EvalContext& ctx);


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
// More granular phase values for smoother tapering between MG and EG
// ============================================================================

// Phase values per piece type - higher = more middlegame character
// Total phase = sum of all non-pawn, non-king pieces at game start
constexpr int PhaseValue[PIECE_TYPE_NB] = {
    0,   // NO_PIECE_TYPE
    0,   // PAWN (pawns don't affect phase)
    1,   // KNIGHT
    1,   // BISHOP
    2,   // ROOK
    4,   // QUEEN
    0    // KING
};

// Total phase at game start: 4*1 (knights) + 4*1 (bishops) + 4*2 (rooks) + 2*4 (queens) = 24
constexpr int TotalPhase = 24;

// Phase thresholds for special evaluations
constexpr int MiddlegamePhase = 18;  // Phase >= 18 is clearly middlegame
constexpr int EndgamePhase = 6;       // Phase <= 6 is clearly endgame

// Helper to interpolate with non-linear scaling for smoother transition
// Returns 0-256 scale (for fixed-point arithmetic)
inline int phase_weight(int phase) {
    // Clamp phase to valid range
    phase = std::max(0, std::min(phase, TotalPhase));
    // Linear interpolation: 0 (pure endgame) to 256 (pure middlegame)
    return (phase * 256 + TotalPhase / 2) / TotalPhase;
}

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

// Candidate Passed Pawn - pawn that can become passed with a single pawn move/exchange
// Bonus scaled by rank (closer to promotion = more valuable)
constexpr EvalScore CandidatePassedBonus[8] = {
    S(  0,   0), S(  3,   5), S(  5,  10), S( 10,  20),
    S( 20,  40), S( 35,  60), S( 50,  90), S(  0,   0)
};

// Rule of the Square bonus (endgame only): bonus for passed pawn that king can't catch
constexpr int RuleOfSquareBonus = 150;  // Significant endgame bonus

// ============================================================================
// Advanced Pawn Structure Constants
// ============================================================================

// Protected passed pawn - a passed pawn defended by another pawn
// Much more dangerous because it's harder to blockade
constexpr EvalScore ProtectedPassedBonus[8] = {
    S(  0,   0), S(  8,  15), S( 15,  30), S( 25,  50),
    S( 45,  90), S( 75, 140), S(110, 200), S(  0,   0)
};

// Pawn island penalty - more islands = weaker structure
// Each island beyond the first incurs a penalty
constexpr EvalScore PawnIslandPenalty = S(-5, -8);

// Pawn chain bonus - pawns protecting each other diagonally
// A strong pawn chain provides both attack and defense
constexpr EvalScore PawnChainBonus = S(5, 3);
constexpr EvalScore PawnChainBaseBonus = S(8, 5);  // Extra for base of chain

// Central pawn presence bonus (d4/e4 or d5/e5)
// Central pawns control key squares
constexpr EvalScore CentralPawnBonus = S(15, 5);

// Pawn duo bonus - two pawns side by side on same rank (neither attacks/defends other)
// Different from phalanx which can attack together
constexpr EvalScore PawnDuoBonus = S(5, 8);

// Lever - pawn that can capture an enemy pawn
// Already exists but adding file-specific weighing
constexpr EvalScore CentralLeverBonus = S(8, 4);  // Lever on central files (c-f)

// Outpost hole penalty - weak squares in our camp that enemy can use
// A hole is a square that cannot be defended by our pawns
constexpr EvalScore OutpostHolePenalty = S(-12, -8);

// Hanging pawn penalty - two isolated pawns on adjacent open files
// They can be targets but also can advance to create threats
constexpr EvalScore HangingPawnPenalty = S(-8, -10);
constexpr EvalScore HangingPawnWithThreat = S(5, 0);  // Bonus if they threaten to advance

// Backward on half-open file - extra penalty when file is semi-open
constexpr EvalScore BackwardOnHalfOpen = S(-10, -5);

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
// Material Imbalance Table
// Bonuses/penalties for piece combinations (from CPW)
// ============================================================================

// Rook pair vs Bishop+Knight imbalance
// Having two rooks is often better than bishop+knight in open positions
constexpr EvalScore RookPairBonus = S(15, 25);

// Bishop+Knight imbalance bonus (against rook pair in closed positions)
constexpr EvalScore BishopKnightCombo = S(10, 5);

// Redundancy penalties (multiple pieces of same type)
// Two knights are less effective together (no complementary mobility)
constexpr EvalScore KnightPairPenalty = S(-8, -10);

// Rooks without queens bonus (endgame rooks are stronger)
constexpr EvalScore RooksWithoutQueens = S(5, 20);

// Queen without rooks penalty (queen alone is weaker in endgame)
constexpr EvalScore QueenWithoutRooks = S(-5, -15);

// Imbalance: extra minor vs rooks deficit
// Having minor pieces vs missing rooks can be compensation
constexpr EvalScore MinorForExchange = S(0, 10);  // When down exchange but have extra minor

// ============================================================================
// King Ring Expansion - Inner vs Outer Ring
// Inner ring: immediately adjacent (8 squares max)
// Outer ring: two squares away (16 squares max)
// ============================================================================

// Attack weight multipliers for inner vs outer ring
constexpr int InnerRingAttackWeight = 3;  // Attacks on inner ring are 3x as dangerous
constexpr int OuterRingAttackWeight = 1;  // Attacks on outer ring are baseline

// King zone weak square penalties (undefended squares near king)
constexpr int InnerRingWeakSquarePenalty = 15;  // MG penalty per weak inner ring square
constexpr int OuterRingWeakSquarePenalty = 5;   // MG penalty per weak outer ring square

// ============================================================================
// Pawn Lever Detection
// A pawn lever is when our pawn can attack enemy pawn to open file near king
// ============================================================================

// Bonus for having a pawn lever against enemy king position
constexpr EvalScore PawnLeverBonus = S(20, 5);   // Attacking pawn storm threat
constexpr EvalScore PawnLeverOnKingFile = S(30, 8);  // Lever on file near enemy king

// ============================================================================
// Minor Piece Coordination
// Bonuses for piece combinations that work well together
// ============================================================================

// Queen + Knight is often stronger than Queen + Bishop (fork potential)
constexpr EvalScore QueenKnightSynergy = S(15, 10);

// Bishop + Knight controlling same color complex
constexpr EvalScore BishopKnightCoordination = S(12, 8);

// Knights close together (can defend each other, create threats)
constexpr EvalScore KnightProximityBonus = S(8, 5);

// Bishops on opposite colors with both rooks (battery potential)
constexpr EvalScore BishopColorComplex = S(5, 10);

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

// New evaluation components
EvalScore eval_material_imbalance(const Board& board, Color c);
EvalScore eval_pawn_levers(const Board& board, Color c, EvalContext& ctx);
EvalScore eval_minor_coordination(const Board& board, Color c, EvalContext& ctx);

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
