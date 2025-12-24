#ifndef TUNING_HPP
#define TUNING_HPP

#include "types.hpp"

namespace Tuning {

    // ========================================================================
    // Tunable Parameters
    // ========================================================================

    // Material Values
    extern EvalScore PawnValue;
    extern EvalScore KnightValue;
    extern EvalScore BishopValue;
    extern EvalScore RookValue;
    extern EvalScore QueenValue;

    // Piece Activity Bonuses
    extern EvalScore BishopPairBonus;
    extern EvalScore RookOpenFileBonus;
    extern EvalScore RookSemiOpenFileBonus;
    extern EvalScore RookOnSeventhBonus;
    extern EvalScore KnightOutpostBonus;

    // Pawn Structure
    extern EvalScore IsolatedPawnPenalty;
    extern EvalScore DoubledPawnPenalty;
    extern EvalScore BackwardPawnPenalty;
    extern EvalScore ConnectedPawnBonus;
    extern EvalScore PhalanxBonus;

    // King Safety
    extern int KingSafetyWeight;  // Scale factor (percentage, default 83)

    // Initialization
    void init();
}

#endif // TUNING_HPP
