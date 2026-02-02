#ifndef SEARCH_CONSTANTS_HPP
#define SEARCH_CONSTANTS_HPP

#include <algorithm>
namespace SearchParams {

// ============================================================================
// DYNAMIC PARAMETER FUNCTIONS (Stockfish-style)
// These functions compute parameters based on position characteristics
// ============================================================================

inline int futility_margin(int depth, bool improving) {
    return 150 * depth - (improving ? 50 : 0);
}

inline int lmp_threshold(int depth, bool improving) {
    int base = 2 + depth * 2;
    return improving ? base : base + 2;
}

inline int razoring_margin(int depth) {
    return 400 + 150 * depth;
}

inline int rfp_margin(int depth, bool improving) {
    return 95 * depth - (improving ? 50 : 0);
}

// ============================================================================
// Pruning Depths
// ============================================================================

constexpr int FUTILITY_MAX_DEPTH = 6;
constexpr int RAZORING_MAX_DEPTH = 3;
constexpr int RFP_MAX_DEPTH = 6;
constexpr int LMP_MAX_DEPTH = 7;
constexpr int SEE_QUIET_MAX_DEPTH = 3;
constexpr int SEE_CAPTURE_MAX_DEPTH = 4;

constexpr int SEE_CAPTURE_THRESHOLD_PER_DEPTH = -25;
constexpr int SEE_QUIET_THRESHOLD_PER_DEPTH = -60;

constexpr int HISTORY_LEAF_PRUNING_DEPTH = 5;
constexpr int HISTORY_LEAF_PRUNING_MARGIN = 6000;

constexpr int COUNTER_HIST_PRUNING_DEPTH = 4;
constexpr int COUNTER_HIST_PRUNING_MARGIN = 3000;

constexpr int FOLLOWUP_HIST_PRUNING_DEPTH = 3;
constexpr int FOLLOWUP_HIST_PRUNING_MARGIN = 4000;

// ============================================================================
// Extension Parameters
// ============================================================================

constexpr int MAX_EXTENSIONS = 5;

constexpr int SINGULAR_DEPTH = 4;
constexpr int SINGULAR_MARGIN = 64;
constexpr int SINGULAR_TT_DEPTH_PENALTY = 8;
constexpr int SINGULAR_IMPROVING_BONUS = 10;
constexpr int SINGULAR_DOUBLE_EXT_BASE = 60;

constexpr int CAPTURE_EXT_MIN_DEPTH = 8;
constexpr int CAPTURE_EXT_SEE_THRESHOLD = 0;

constexpr int MATE_THREAT_EXT_MIN_DEPTH = 4;

constexpr int PV_EXT_MIN_DEPTH = 7;

// ============================================================================
// Multi-Cut Parameters
// ============================================================================

constexpr int MULTI_CUT_DEPTH = 12;
constexpr int MULTI_CUT_COUNT = 3;
constexpr int MULTI_CUT_REQUIRED = 2;

// ============================================================================
// ProbCut Parameters
// ============================================================================

constexpr int PROBCUT_DEPTH = 64;
constexpr int PROBCUT_MARGIN = 200;

// ============================================================================
// Null Move Parameters
// ============================================================================

constexpr int NULL_MOVE_MIN_DEPTH = 3;
constexpr int NULL_MOVE_VERIFY_DEPTH = 16;

// ============================================================================
// Aspiration Window Parameters
// ============================================================================

constexpr int ASPIRATION_INITIAL_DELTA = 12;
constexpr int ASPIRATION_MIN_DEPTH = 5;

// ============================================================================
// IIR (Internal Iterative Reductions) Parameters
// ============================================================================

constexpr int IIR_MIN_DEPTH = 3;
constexpr int IIR_REDUCTION = 1;
constexpr int IIR_PV_REDUCTION = 1;
constexpr int IIR_CUT_REDUCTION = 2;

// ============================================================================
// Quiescence Search Parameters
// ============================================================================

constexpr int QSEARCH_CHECK_DEPTH = -2;
constexpr int DELTA_PRUNING_MARGIN = 550;

// ============================================================================
// LMR Tuning Parameters (Adjusted for HCE Engines)
// HCE engine needs slightly less aggressive reductions due to eval noise
// ============================================================================

constexpr double LMR_BASE = 0.85;
constexpr double LMR_DIVISOR = 1.5;

constexpr int LMR_CUTNODE_BONUS = 3;
constexpr int LMR_CUTOFF_CNT_BONUS = 1;
constexpr int LMR_ALLNODE_SCALE = 1;

// ============================================================================
// Extension Control Parameters
// ============================================================================

constexpr int DOUBLE_EXT_LIMIT = 3;
constexpr int TRIPLE_EXT_LIMIT = 2;       // Limit for triple extensions
constexpr int MAX_EXTENSION_PLY_RATIO = 2;

constexpr int NEG_EXT_THRESHOLD = 100;
constexpr int NEG_EXT_MIN_DEPTH = 6;

// Triple Extension Parameters (PlentyChess-style)
constexpr int SINGULAR_DOUBLE_EXT_MARGIN = 6;
constexpr int SINGULAR_TRIPLE_EXT_MARGIN = 41;
constexpr int SINGULAR_DEPTH_INCREASE = 10;

// ============================================================================
// Fractional Extension System
// Uses 100-point scale: 100 = 1 full ply, 50 = 0.5 ply, etc.
// Accumulates fractional extensions and converts to full when >= 100
// ============================================================================

constexpr int FRAC_EXT_SCALE = 100;           // 100 points = 1 full ply

// Fractional extension values (in centiplies, 100 = 1 ply)
constexpr int FRAC_EXT_CHECK = 100;           // Check extension: 1.0 ply (safe check at high depth)
constexpr int FRAC_EXT_CHECK_PARTIAL = 50;    // Partial check extension: 0.5 ply
constexpr int FRAC_EXT_PASSED_PAWN = 100;     // Passed pawn to 7th rank: 1.0 ply
constexpr int FRAC_EXT_RECAPTURE = 50;        // Recapture: 0.5 ply
constexpr int FRAC_EXT_SINGULAR = 100;        // Singular move: 1.0 ply
constexpr int FRAC_EXT_SINGULAR_DOUBLE = 200; // Double singular: 2.0 ply
constexpr int FRAC_EXT_PV_MOVE = 50;          // First PV move: 0.5 ply
constexpr int FRAC_EXT_MATE_THREAT = 75;      // Mate threat detected: 0.75 ply
constexpr int FRAC_EXT_CAPTURE_IMPORTANT = 50;// Important capture (queen): 0.5 ply

// ============================================================================
// Post-LMR Adjustment Parameters
// Uses STANDARD PLY system (1 = 1 ply) - NOT centiplie
// ============================================================================

constexpr int POST_LMR_WORSENING_THRESHOLD = 2;  // LMR reduction must be >= 2 ply
constexpr int POST_LMR_WORSENING_REDUCTION = 1;  // Additional 1 ply reduction
constexpr int POST_LMR_MIN_DEPTH = 3;            // Only apply at depth >= 3

// ============================================================================
// Dynamic SEE Thresholds
// ============================================================================

constexpr int SEE_CAPTURE_IMPROVING_FACTOR = 15;
constexpr int SEE_CAPTURE_NOT_IMPROVING_FACTOR = 30;
constexpr int SEE_QUIET_IMPROVING_FACTOR = 35;
constexpr int SEE_QUIET_NOT_IMPROVING_FACTOR = 60;

// ============================================================================
// History & LMR Weights (Stockfish-style)
// ============================================================================

constexpr int HISTORY_LMR_DIVISOR = 3500;
constexpr int HISTORY_LMR_MAX_ADJ = 6;

constexpr int HISTORY_BONUS_BASE = 105;
constexpr int HISTORY_BONUS_LINEAR = 175;
constexpr int HISTORY_BONUS_QUADRATIC = 11;
constexpr int HISTORY_BONUS_MAX = 2400;

constexpr int HISTORY_MALUS_BASE = 80;
constexpr int HISTORY_MALUS_LINEAR = 145;
constexpr int HISTORY_MALUS_QUADRATIC = 8;
constexpr int HISTORY_MALUS_MAX = 1900;

constexpr int CONT_HIST_1PLY_WEIGHT = 2;
constexpr int CONT_HIST_2PLY_WEIGHT = 1;
constexpr int CONT_HIST_4PLY_WEIGHT = 1;

constexpr int CAPTURE_HIST_BONUS_SCALE = 7;
constexpr int CAPTURE_HIST_MALUS_SCALE = 5;

// stat_bonus/malus take depth in milli-ply
inline int stat_bonus(int depth) {
    return std::min(HISTORY_BONUS_BASE + HISTORY_BONUS_LINEAR * depth + HISTORY_BONUS_QUADRATIC * depth * depth,
                    HISTORY_BONUS_MAX);
}

inline int stat_malus(int depth) {
    return std::min(HISTORY_MALUS_BASE + HISTORY_MALUS_LINEAR * depth + HISTORY_MALUS_QUADRATIC * depth * depth,
                    HISTORY_MALUS_MAX);
}

} // namespace SearchParams

#endif // SEARCH_CONSTANTS_HPP
