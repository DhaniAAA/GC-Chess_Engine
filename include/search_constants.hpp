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
// LMR Tuning Parameters (Stockfish-style)
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
constexpr int MAX_EXTENSION_PLY_RATIO = 2;

constexpr int NEG_EXT_THRESHOLD = 100;
constexpr int NEG_EXT_MIN_DEPTH = 6;

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
