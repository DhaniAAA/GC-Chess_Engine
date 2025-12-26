#include "tuning.hpp"

namespace Tuning {

    // ========================================================================
    // Tunable Parameters - Texel Tuned Values
    // Optimized using 100K labeled positions (1.95% improvement)
    // ========================================================================

    // Material Values (Texel Tuned)
    EvalScore PawnValue   = S( 60,  80);
    EvalScore KnightValue = S(200, 200);
    EvalScore BishopValue = S(200, 200);
    EvalScore RookValue   = S(350, 400);
    EvalScore QueenValue  = S(700, 800);

    // Piece Activity Bonuses (Texel Tuned)
    EvalScore BishopPairBonus       = S( 0,  0);
    EvalScore RookOpenFileBonus     = S( 0,  0);
    EvalScore RookSemiOpenFileBonus = S( 11,  3);
    EvalScore RookOnSeventhBonus    = S( 0,  0);
    EvalScore KnightOutpostBonus    = S( 0,  0);

    // Pawn Structure (Texel Tuned)
    EvalScore IsolatedPawnPenalty   = S(-45,   0);
    EvalScore DoubledPawnPenalty    = S(-16, -21);
    EvalScore BackwardPawnPenalty   = S( 0,   0);
    EvalScore ConnectedPawnBonus    = S( 0,   2);
    EvalScore PhalanxBonus          = S( 0,   0);

    // King Safety
    int KingSafetyWeight = 83;  // Scale factor (percentage)

    void init() {
        // Initialization logic if needed
    }

} // namespace Tuning
