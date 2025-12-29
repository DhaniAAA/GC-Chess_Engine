#include "tuning.hpp"

namespace Tuning {

    // ========================================================================
    // Tunable Parameters - Texel Tuned Values
    // Optimized using 100K labeled positions (1.95% improvement)
    // ========================================================================

    // Material Values (Texel Tuned)
    EvalScore PawnValue   = S(100, 100);
    EvalScore KnightValue = S(320, 330);
    EvalScore BishopValue = S(330, 340);
    EvalScore RookValue   = S(500, 520);
    EvalScore QueenValue  = S(950, 1000);

    // Piece Activity Bonuses (Texel Tuned)
    EvalScore BishopPairBonus       = S(20, 50);
    EvalScore RookOpenFileBonus     = S(40, 20);
    EvalScore RookSemiOpenFileBonus = S(20, 10);
    EvalScore RookOnSeventhBonus    = S(20, 40);
    EvalScore KnightOutpostBonus    = S(30, 20);

    // Pawn Structure (Texel Tuned)
    EvalScore IsolatedPawnPenalty   = S(-10, -20);
    EvalScore DoubledPawnPenalty    = S(-10, -20);
    EvalScore BackwardPawnPenalty   = S(-5,  -10);
    EvalScore ConnectedPawnBonus    = S(15, 15);
    EvalScore PhalanxBonus          = S(10, 20);

    // King Safety
    int KingSafetyWeight = 90;  // Scale factor (percentage)

    void init() {
        // Initialization logic if needed
    }

} // namespace Tuning
