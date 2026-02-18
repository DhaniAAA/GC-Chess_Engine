#ifndef TUNING_HPP
#define TUNING_HPP

#include "types.hpp"

namespace Tuning {

    // ========================================================================
    // Material Values
    // ========================================================================
    extern EvalScore PawnValue;
    extern EvalScore KnightValue;
    extern EvalScore BishopValue;
    extern EvalScore RookValue;
    extern EvalScore QueenValue;

    // ========================================================================
    // Piece Activity Bonuses
    // ========================================================================
    extern EvalScore BishopPairBonus;
    extern EvalScore RookOpenFileBonus;
    extern EvalScore RookSemiOpenFileBonus;
    extern EvalScore RookOnSeventhBonus;
    extern EvalScore KnightOutpostBonus;

    // ========================================================================
    // Pawn Structure
    // ========================================================================
    extern EvalScore IsolatedPawnPenalty;
    extern EvalScore DoubledPawnPenalty;
    extern EvalScore BackwardPawnPenalty;
    extern EvalScore ConnectedPawnBonus;
    extern EvalScore PhalanxBonus;

    extern EvalScore PawnIslandPenalty;
    extern EvalScore PawnChainBonus;
    extern EvalScore PawnChainBaseBonus;
    extern EvalScore CentralPawnBonus;
    extern EvalScore PawnDuoBonus;
    extern EvalScore OutpostHolePenalty;
    extern EvalScore HangingPawnPenalty;
    extern EvalScore BackwardOnHalfOpen;
    extern EvalScore CentralLeverBonus;

    // ========================================================================
    // Passed Pawn Bonuses (by rank)
    // ========================================================================
    extern EvalScore PassedPawnBonus[8];
    extern EvalScore ConnectedPassedBonus[8];
    extern EvalScore ProtectedPassedBonus[8];
    extern EvalScore CandidatePassedBonus[8];
    extern int RuleOfSquareBonus;

    // ========================================================================
    // Mobility Tables
    // ========================================================================
    extern EvalScore KnightMobility[9];
    extern EvalScore BishopMobility[14];
    extern EvalScore RookMobility[15];
    extern EvalScore QueenMobility[28];

    // ========================================================================
    // King Safety
    // ========================================================================
    extern int KingSafetyWeight;
    extern int KnightAttackWeight;
    extern int BishopAttackWeight;
    extern int RookAttackWeight;
    extern int QueenAttackWeight;
    extern int InnerRingAttackWeight;
    extern int OuterRingAttackWeight;
    extern EvalScore KingSemiOpenFilePenalty;
    extern EvalScore KingOpenFilePenalty;
    extern int PawnShieldBonus[4];

    // ========================================================================
    // Piece Activity
    // ========================================================================
    extern EvalScore KnightOnRim;
    extern EvalScore BishopLongDiagonal;
    extern EvalScore RookBehindPasser;
    extern EvalScore TrappedBishopPenalty;
    extern EvalScore TrappedRookPenalty;
    extern EvalScore HighMobilityBonus;
    extern EvalScore LowMobilityPenalty;

    // ========================================================================
    // Material Imbalance
    // ========================================================================
    extern EvalScore RookPairBonus;
    extern EvalScore BishopKnightCombo;
    extern EvalScore KnightPairPenalty;
    extern EvalScore RooksWithoutQueens;
    extern EvalScore QueenWithoutRooks;

    // ========================================================================
    // King Safety Extended
    // ========================================================================
    extern int SafeCheckBonus[7];         // indexed by PieceType (0=none,1=pawn,...6=king)
    extern int ContactCheckBonus;
    extern int KingTropismWeight[7];      // indexed by PieceType
    extern int PawnShieldQuality[5][4];   // [file_rel_king][rank_distance]
    extern int PawnStormDanger[5][4];     // [file_rel_king][rank_distance]
    extern EvalScore ShelterWeakness[4];  // [weakness_level]

    // ========================================================================
    // Piece Patterns
    // ========================================================================
    extern EvalScore FianchettoBonus;
    extern EvalScore TrappedKnightPenalty;

    // Initialization
    void init();
}

#endif // TUNING_HPP
