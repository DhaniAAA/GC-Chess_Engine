#ifndef EVAL_HPP
#define EVAL_HPP

#include "board.hpp"
#include "magic.hpp"
#include <algorithm>
#include <cstring>
#include "tuning.hpp"

namespace Eval {

constexpr int ATTACK_PIECE_TYPES = PIECE_TYPE_NB + 1;
constexpr int ALL_PIECES = PIECE_TYPE_NB;

struct EvalContext {
    Bitboard attackedBy[COLOR_NB][ATTACK_PIECE_TYPES];
    Bitboard attackedBy2[COLOR_NB];
    Bitboard kingRing[COLOR_NB];
    Bitboard innerKingRing[COLOR_NB];
    Bitboard outerKingRing[COLOR_NB];
    Square kingSquare[COLOR_NB];
    int kingAttackersCount[COLOR_NB];
    int kingAttackersWeight[COLOR_NB];
    int innerRingAttacks[COLOR_NB];
    int outerRingAttacks[COLOR_NB];
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

using Tuning::PawnValue;
using Tuning::KnightValue;
using Tuning::BishopValue;
using Tuning::RookValue;
using Tuning::QueenValue;

constexpr int PhaseValue[PIECE_TYPE_NB] = {
    0,
    0,
    1,
    1,
    2,
    4,
    0
};

constexpr int TotalPhase = 24;

constexpr int MiddlegamePhase = 18;
constexpr int EndgamePhase = 6;

inline int phase_weight(int phase) {
    phase = std::max(0, std::min(phase, TotalPhase));
    return (phase * 256 + TotalPhase / 2) / TotalPhase;
}

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

constexpr EvalScore CandidatePassedBonus[8] = {
    S(  0,   0), S(  3,   5), S(  5,  10), S( 10,  20),
    S( 20,  40), S( 35,  60), S( 50,  90), S(  0,   0)
};

constexpr int RuleOfSquareBonus = 150;

constexpr EvalScore ProtectedPassedBonus[8] = {
    S(  0,   0), S(  8,  15), S( 15,  30), S( 25,  50),
    S( 45,  90), S( 75, 140), S(110, 200), S(  0,   0)
};

constexpr EvalScore PawnIslandPenalty = S(-5, -8);

constexpr EvalScore PawnChainBonus = S(5, 3);
constexpr EvalScore PawnChainBaseBonus = S(8, 5);
constexpr EvalScore CentralPawnBonus = S(15, 5);

constexpr EvalScore PawnDuoBonus = S(5, 8);

constexpr EvalScore CentralLeverBonus = S(8, 4);

constexpr EvalScore OutpostHolePenalty = S(-12, -8);

constexpr EvalScore HangingPawnPenalty = S(-8, -10);
constexpr EvalScore HangingPawnWithThreat = S(5, 0);

constexpr EvalScore BackwardOnHalfOpen = S(-10, -5);
constexpr EvalScore KingSemiOpenFilePenalty = S( 15, 0);
constexpr EvalScore KingOpenFilePenalty     = S( 25, 0);

using Tuning::BishopPairBonus;
using Tuning::RookOpenFileBonus;
using Tuning::RookSemiOpenFileBonus;
using Tuning::RookOnSeventhBonus;
using Tuning::KnightOutpostBonus;

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

constexpr int KnightAttackWeight = 2;
constexpr int BishopAttackWeight = 2;
constexpr int RookAttackWeight   = 3;
constexpr int QueenAttackWeight  = 5;

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

constexpr int PawnShieldBonus[4] = { 0, 10, 20, 30 };

constexpr EvalScore RookPairBonus = S(15, 25);

constexpr EvalScore BishopKnightCombo = S(10, 5);

constexpr EvalScore KnightPairPenalty = S(-8, -10);

constexpr EvalScore RooksWithoutQueens = S(5, 20);

constexpr EvalScore QueenWithoutRooks = S(-5, -15);

constexpr EvalScore MinorForExchange = S(0, 10);

constexpr int InnerRingAttackWeight = 3;
constexpr int OuterRingAttackWeight = 1;

constexpr int InnerRingWeakSquarePenalty = 15;
constexpr int OuterRingWeakSquarePenalty = 5;

constexpr EvalScore PawnLeverBonus = S(20, 5);
constexpr EvalScore PawnLeverOnKingFile = S(30, 8);

constexpr EvalScore QueenKnightSynergy = S(15, 10);

constexpr EvalScore BishopKnightCoordination = S(12, 8);
constexpr EvalScore KnightProximityBonus = S(8, 5);

constexpr EvalScore BishopColorComplex = S(5, 10);

constexpr EvalScore KnightCentralization[4] = {
    S(  0,   0),
    S(  5,   3),
    S( 12,   8),
    S( 20,  12)
};

constexpr EvalScore BishopCentralization[4] = {
    S(  0,   0),
    S(  3,   2),
    S(  8,   5),
    S( 12,   8)
};
constexpr EvalScore RookCentralization[4] = {
    S(  0,   0),
    S(  3,   2),
    S(  6,   4),
    S( 10,   6)
};

constexpr EvalScore QueenCentralization[4] = {
    S( -5,   0),
    S(  0,   2),
    S(  3,   5),
    S(  5,   8)
};

constexpr EvalScore HighMobilityBonus = S(10, 15);
constexpr EvalScore LowMobilityPenalty = S(-15, -20);

constexpr EvalScore KnightOnRim = S(-10, -8);
constexpr EvalScore BishopLongDiagonal = S(15, 10);
constexpr EvalScore RookBehindPasser = S(20, 30);
constexpr EvalScore RookOnQueenFile = S(8, 5);
constexpr EvalScore QueenEarlyDevelopment = S(-15, 0);

constexpr EvalScore FianchettoBonus = S(12, 8);
constexpr EvalScore FianchettoBroken = S(-10, -5);
constexpr EvalScore TrappedBishopPenalty = S(-100, -80);
constexpr EvalScore TrappedRookPenalty = S(-50, -30);
constexpr EvalScore TrappedKnightPenalty = S(-40, -30);

constexpr int PawnShieldQuality[5][4] = {
    {  -35,       0,        -10,       -20  },
    {  -25,      10,          0,       -10  },
    {  -30,      15,          5,       -15  },
    {  -25,      10,          0,       -10  },
    {  -35,       0,        -10,       -20  },
};

constexpr int PawnStormDanger[5][4] = {
    {    0,         5,           15,          30   },
    {    0,        10,           25,          45   },
    {    0,        15,           35,          60   },
    {    0,        10,           25,          45   },
    {    0,         5,           15,          30   },
};

constexpr int AttackWeightByPiece[PIECE_TYPE_NB] = {
    0,
    0,
    3,
    2,
    4,
    6,
    0
};

constexpr int MinAttackersForDanger = 2;
constexpr int MaxKingSafetyPenalty = 800;
constexpr int KingExposedOnFile = 20;
constexpr int KingExposedOnDiagonal = 15;
constexpr int KingNoQueensideCastle = 25;
constexpr int KingNoKingsideCastle = 30;
constexpr EvalScore ShelterWeakness[4] = {
    S(  0,   0),
    S(-10,  -3),
    S(-25,  -8),
    S(-50, -15)
};
constexpr int SafeCheckBonus[PIECE_TYPE_NB] = {
    0,
    0,
    45,
    35,
    50,
    60,
    0
};

constexpr int ContactCheckBonus = 40;

constexpr int KingTropismWeight[PIECE_TYPE_NB] = {
    0,
    0,
    5,
    3,
    4,
    2,
    0
};
constexpr int VirtualMobilityWeight = 3;

Square flip_square(Square sq);
Bitboard file_bb(File f);
Bitboard adjacent_files_bb(File f);
Bitboard rank_bb_eval(Rank r);
Bitboard pawn_front_span(Color c, Square s);
Bitboard passed_pawn_mask(Color c, Square s);
Square pawn_push(Color c, Square s);
bool is_backward_pawn(Color c, Square s, Bitboard ourPawns, Bitboard theirPawns);

EvalScore eval_material_pst(const Board& board, Color c);
EvalScore eval_pawn_structure(const Board& board, Color c);
EvalScore eval_pieces(const Board& board, Color c);
EvalScore eval_king_safety(const Board& board, Color c);
EvalScore eval_space(const Board& board, Color c);

EvalScore eval_material_imbalance(const Board& board, Color c);
EvalScore eval_pawn_levers(const Board& board, Color c, EvalContext& ctx);
EvalScore eval_minor_coordination(const Board& board, Color c, EvalContext& ctx);

EvalScore eval_piece_activity(const Board& board, Color c, EvalContext& ctx);

EvalScore eval_king_safety_advanced(const Board& board, Color c, EvalContext& ctx);

int get_centralization_index(Square sq);
int get_file_centralization(File f);

inline EvalScore piece_pst_score(Piece pc, Square sq) {
    Color c = color_of(pc);
    PieceType pt = type_of(pc);
    Square psq = c == WHITE ? sq : Square(sq ^ 56);

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

struct PawnEntry {
    Key key;
    EvalScore score;

    bool match(Key k) const { return key == k; }
};

class PawnTable {
public:
    static constexpr int SIZE = 16384;
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

int evaluate(const Board& board, int alpha, int beta);
int evaluate(const Board& board);
int evaluate_no_cache(const Board& board);
int material_balance(const Board& board);

}

#endif
