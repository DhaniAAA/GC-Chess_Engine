#include "tuning.hpp"

namespace Tuning {

    // ========================================================================
    // Tunable Parameters Defaults
    // ========================================================================

    // Material Values
    EvalScore PawnValue   = S(100, 120);
    EvalScore KnightValue = S(320, 300);
    EvalScore BishopValue = S(330, 320);
    EvalScore RookValue   = S(500, 550);
    EvalScore QueenValue  = S(950, 1000);

    // Piece Activity Bonuses
    EvalScore BishopPairBonus       = S( 35,  55);
    EvalScore RookOpenFileBonus     = S( 25,  15);
    EvalScore RookSemiOpenFileBonus = S( 12,   8);
    EvalScore RookOnSeventhBonus    = S( 20,  40);
    EvalScore KnightOutpostBonus    = S( 30,  20);

    // Pawn Structure
    EvalScore IsolatedPawnPenalty   = S(-15, -20);
    EvalScore DoubledPawnPenalty    = S(-10, -25);
    EvalScore BackwardPawnPenalty   = S(-10, -15);
    EvalScore ConnectedPawnBonus    = S( 10,  10);
    EvalScore PhalanxBonus          = S( 10,  15);

    // King Safety
    int KingSafetyWeight = 83;  // Scale factor (percentage)

    void init() {
        // Initialization logic if needed
    }

} // namespace Tuning
