#include "tuning.hpp"

namespace Tuning {

    // ========================================================================
    // Material Values
    // ========================================================================
    EvalScore PawnValue   = S(  70,   80);
    EvalScore KnightValue = S( 293,  300);
    EvalScore BishopValue = S( 290,  300);
    EvalScore RookValue   = S( 450,  470);
    EvalScore QueenValue  = S( 880,  900);

    // ========================================================================
    // Piece Activity Bonuses
    // ========================================================================
    EvalScore BishopPairBonus       = S(   0,   19);
    EvalScore RookOpenFileBonus     = S(   5,    0);
    EvalScore RookSemiOpenFileBonus = S(   0,    1);
    EvalScore RookOnSeventhBonus    = S(   0,    0);
    EvalScore KnightOutpostBonus    = S(  20,   13);

    // ========================================================================
    // Pawn Structure
    // ========================================================================
    EvalScore IsolatedPawnPenalty = S(   0,  -40);
    EvalScore DoubledPawnPenalty  = S( -10,  -24);
    EvalScore BackwardPawnPenalty = S(  -3,   -9);
    EvalScore ConnectedPawnBonus  = S(   0,    0);
    EvalScore PhalanxBonus        = S(   0,    0);

    EvalScore PawnIslandPenalty   = S( -20,  -20);
    EvalScore PawnChainBonus      = S(   0,    0);
    EvalScore PawnChainBaseBonus  = S(   8,    5);
    EvalScore CentralPawnBonus    = S(   0,    0);
    EvalScore PawnDuoBonus        = S(   0,    0);
    EvalScore OutpostHolePenalty  = S(   0,    0);
    EvalScore HangingPawnPenalty  = S( -11,  -22);
    EvalScore BackwardOnHalfOpen  = S(  -8,   -6);
    EvalScore CentralLeverBonus   = S(   8,    4);

    // ========================================================================
    // Passed Pawn Bonuses (by rank: 0-7)
    // ========================================================================
    EvalScore PassedPawnBonus[8] = {
        S(   0,   0), S(   0,   0), S(   0,   0), S(   0,   0),
        S(  20,   0), S(  44,  34), S(  79, 118), S(   0,   0)
    };
    EvalScore ConnectedPassedBonus[8] = {
        S(   0,   0), S(   0,   0), S(   0,   0), S(   4,   0),
        S(  16,   5), S(  35,  43), S(  58,  93), S(   0,   0)
    };
    EvalScore ProtectedPassedBonus[8] = {
        S(   0,   0), S(   8,  15), S(   8,  15), S(  17,  20),
        S(  36,  60), S(  71, 121), S( 109, 194), S(   0,   0)
    };
    EvalScore CandidatePassedBonus[8] = {
        S(   0,   0), S(   3,   5), S(   3,   5), S(   3,   5),
        S(   8,  12), S(  26,  52), S(  50,  90), S(   0,   0)
    };
    int RuleOfSquareBonus = 50;

    // ========================================================================
    // Mobility Tables
    // ========================================================================
    EvalScore KnightMobility[9] = {
        S( -55, -60), S( -15, -20), S(  -5, -10), S(   0,   0),
        S(   5,   5), S(  10,  10), S(  15,  15), S(  18,  18),
        S(  20,  20)
    };
    EvalScore BishopMobility[14] = {
        S( -16, -31), S( -16, -17), S(  -9, -11), S(  -9, -10),
        S(  -9,  -8), S(  -6,  -8), S(  -2,  -7), S(  -2,  -7),
        S(   6,  -1), S(  12,  -1), S(  16,   8), S(  20,   8),
        S(  25,  23), S(  25,  23)
    };
    EvalScore RookMobility[15] = {
        S( -16, -25), S( -16, -24), S( -16, -18), S(  -8,  -4),
        S(  -3,  -4), S(   1,  -1), S(   1,  -1), S(   1,  -1),
        S(   1,  -1), S(   1,  -1), S(   3,  -1), S(   7,  -1),
        S(  10,  -1), S(  14,   0), S(  14,   0)
    };
    EvalScore QueenMobility[28] = {
        S( -14, -25), S(  -9, -15), S(  -9, -11), S(  -8,  -3),
        S(  -8,  -1), S(  -8,  -1), S(  -8,  -1), S(  -8,  -1),
        S(  -8,  -1), S(  -8,  -1), S(  -8,  -1), S(  -6,  -1),
        S(  -6,  -1), S(  -6,  -1), S(  -6,  -1), S(  -3,  -1),
        S(  -3,  -1), S(  -2,  -1), S(   0,  -1), S(   2,  -1),
        S(   3,  -1), S(   3,  -1), S(  10,   2), S(  10,   2),
        S(  14,  10), S(  14,  10), S(  15,  18), S(  16,  18)
    };

    // ========================================================================
    // King Safety
    // ========================================================================
    int KingSafetyWeight = 90;
    int KnightAttackWeight = 2;
    int BishopAttackWeight = 2;
    int RookAttackWeight   = 3;
    int QueenAttackWeight  = 5;
    int InnerRingAttackWeight = 1;
    int OuterRingAttackWeight = 1;
    EvalScore KingSemiOpenFilePenalty = S(  15,    0);
    EvalScore KingOpenFilePenalty     = S(  25,    0);
    int PawnShieldBonus[4] = { 0, 10, 22, 19 };

    // ========================================================================
    // Piece Activity
    // ========================================================================
    EvalScore KnightOnRim          = S( -10,   -8);
    EvalScore BishopLongDiagonal   = S(  15,   10);
    EvalScore RookBehindPasser     = S(  20,   30);
    EvalScore TrappedBishopPenalty = S(-100,  -80);
    EvalScore TrappedRookPenalty   = S( -50,  -30);
    EvalScore HighMobilityBonus    = S(  10,   15);
    EvalScore LowMobilityPenalty   = S( -15,  -20);

    // ========================================================================
    // Material Imbalance
    // ========================================================================
    EvalScore RookPairBonus        = S(   0,    0);
    EvalScore BishopKnightCombo    = S(   0,    0);
    EvalScore KnightPairPenalty    = S( -21,   -4);
    EvalScore RooksWithoutQueens   = S(   0,    0);
    EvalScore QueenWithoutRooks    = S( -20,  -30);

    // ========================================================================
    // King Safety Extended
    // ========================================================================
    int SafeCheckBonus[7] = {
        0, 0, 45, 35, 50, 60, 0
    };
    int ContactCheckBonus = 40;

    int KingTropismWeight[7] = {
        0, 0, 5, 3, 4, 2, 0
    };

    int PawnShieldQuality[5][4] = {
        {  -35,    0,  -10,  -20 },
        {  -25,   10,    0,  -10 },
        {  -30,   15,    5,  -15 },
        {  -25,   10,    0,  -10 },
        {  -35,    0,  -10,  -20 },
    };

    int PawnStormDanger[5][4] = {
        {    0,    5,   15,   30 },
        {    0,   10,   25,   45 },
        {    0,   15,   35,   60 },
        {    0,   10,   25,   45 },
        {    0,    5,   15,   30 },
    };

    EvalScore ShelterWeakness[4] = {
        S(   0,   0), S( -10,  -3), S( -25,  -8), S( -50, -15)
    };

    // ========================================================================
    // Piece Patterns
    // ========================================================================
    EvalScore FianchettoBonus      = S(  12,    8);
    EvalScore TrappedKnightPenalty = S( -40,  -30);

    void init() {
    }

}
