#include "tuning.hpp"

namespace Tuning {

    // ========================================================================
    // Material Values
    // ========================================================================
    EvalScore PawnValue   = S(100, 100);
    EvalScore KnightValue = S(320, 330);
    EvalScore BishopValue = S(330, 340);
    EvalScore RookValue   = S(500, 520);
    EvalScore QueenValue  = S(950, 1000);

    // ========================================================================
    // Piece Activity Bonuses
    // ========================================================================
    EvalScore BishopPairBonus       = S(20, 50);
    EvalScore RookOpenFileBonus     = S(40, 20);
    EvalScore RookSemiOpenFileBonus = S(20, 10);
    EvalScore RookOnSeventhBonus    = S(20, 40);
    EvalScore KnightOutpostBonus    = S(30, 20);

    // ========================================================================
    // Pawn Structure
    // ========================================================================
    EvalScore IsolatedPawnPenalty   = S(-10, -20);
    EvalScore DoubledPawnPenalty    = S(-10, -20);
    EvalScore BackwardPawnPenalty   = S(-5,  -10);
    EvalScore ConnectedPawnBonus    = S(15, 15);
    EvalScore PhalanxBonus          = S(10, 20);

    EvalScore PawnIslandPenalty     = S(-5, -8);
    EvalScore PawnChainBonus        = S(5, 3);
    EvalScore PawnChainBaseBonus    = S(8, 5);
    EvalScore CentralPawnBonus      = S(15, 5);
    EvalScore PawnDuoBonus          = S(5, 8);
    EvalScore OutpostHolePenalty    = S(-12, -8);
    EvalScore HangingPawnPenalty    = S(-8, -10);
    EvalScore BackwardOnHalfOpen    = S(-10, -5);
    EvalScore CentralLeverBonus     = S(8, 4);

    // ========================================================================
    // Passed Pawn Bonuses (by rank: 0-7)
    // ========================================================================
    EvalScore PassedPawnBonus[8] = {
        S(  0,   0), S(  5,  10), S( 10,  20), S( 20,  40),
        S( 40,  75), S( 70, 120), S(100, 180), S(  0,   0)
    };
    EvalScore ConnectedPassedBonus[8] = {
        S(  0,   0), S(  5,   8), S( 10,  15), S( 15,  25),
        S( 25,  45), S( 40,  70), S( 60, 100), S(  0,   0)
    };
    EvalScore ProtectedPassedBonus[8] = {
        S(  0,   0), S(  8,  15), S( 15,  30), S( 25,  50),
        S( 45,  90), S( 75, 140), S(110, 200), S(  0,   0)
    };
    EvalScore CandidatePassedBonus[8] = {
        S(  0,   0), S(  3,   5), S(  5,  10), S( 10,  20),
        S( 20,  40), S( 35,  60), S( 50,  90), S(  0,   0)
    };
    int RuleOfSquareBonus = 150;

    // ========================================================================
    // Mobility Tables
    // ========================================================================
    EvalScore KnightMobility[9] = {
        S(-30, -40), S(-15, -20), S( -5, -10), S(  0,  0),
        S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
        S( 20,  20)
    };
    EvalScore BishopMobility[14] = {
        S(-25, -35), S(-15, -20), S( -5, -10), S(  0,  0),
        S(  5,   5), S( 10,  10), S( 15,  15), S( 18, 18),
        S( 20,  20), S( 22,  22), S( 24,  24), S( 25, 25),
        S( 26,  26), S( 27,  27)
    };
    EvalScore RookMobility[15] = {
        S(-20, -30), S(-12, -18), S( -5, -10), S(  0,  0),
        S(  5,   5), S(  8,  10), S( 10,  15), S( 12, 18),
        S( 14,  20), S( 16,  22), S( 17,  24), S( 18, 25),
        S( 19,  26), S( 20,  27), S( 20,  28)
    };
    EvalScore QueenMobility[28] = {
        S(-15, -25), S(-10, -15), S( -5, -10), S(  0,  0),
        S(  2,   3), S(  4,   5), S(  5,   7), S(  6,  8),
        S(  7,   9), S(  8,  10), S(  9,  11), S( 10, 12),
        S( 10,  13), S( 11,  13), S( 11,  14), S( 12, 14),
        S( 12,  15), S( 13,  15), S( 13,  16), S( 14, 16),
        S( 14,  17), S( 15,  17), S( 15,  18), S( 15, 18),
        S( 16,  18), S( 16,  19), S( 16,  19), S( 17, 20)
    };

    // ========================================================================
    // King Safety
    // ========================================================================
    int KingSafetyWeight = 90;
    int KnightAttackWeight = 2;
    int BishopAttackWeight = 2;
    int RookAttackWeight   = 3;
    int QueenAttackWeight  = 5;
    int InnerRingAttackWeight = 3;
    int OuterRingAttackWeight = 1;
    EvalScore KingSemiOpenFilePenalty = S( 15, 0);
    EvalScore KingOpenFilePenalty     = S( 25, 0);
    int PawnShieldBonus[4] = { 0, 10, 20, 30 };

    // ========================================================================
    // Piece Activity
    // ========================================================================
    EvalScore KnightOnRim          = S(-10, -8);
    EvalScore BishopLongDiagonal   = S(15, 10);
    EvalScore RookBehindPasser     = S(20, 30);
    EvalScore TrappedBishopPenalty = S(-100, -80);
    EvalScore TrappedRookPenalty   = S(-50, -30);
    EvalScore HighMobilityBonus    = S(10, 15);
    EvalScore LowMobilityPenalty   = S(-15, -20);

    // ========================================================================
    // Material Imbalance
    // ========================================================================
    EvalScore RookPairBonus        = S(15, 25);
    EvalScore BishopKnightCombo    = S(10, 5);
    EvalScore KnightPairPenalty    = S(-8, -10);
    EvalScore RooksWithoutQueens   = S(5, 20);
    EvalScore QueenWithoutRooks    = S(-5, -15);

    void init() {
    }

}
