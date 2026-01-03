#ifndef SEARCH_CONSTANTS_HPP
#define SEARCH_CONSTANTS_HPP

#include <algorithm>  // For std::min, std::max in dynamic functions
namespace SearchParams {

// ============================================================================
// DYNAMIC PARAMETER FUNCTIONS (Stockfish-style)
// These functions compute parameters based on position characteristics
// ============================================================================

// Dynamic Futility Margin - looser when improving, tighter when not
inline int futility_margin(int depth, bool improving) {
    return 150 * depth - (improving ? 50 : 0);
}

// Dynamic LMP Threshold - more aggressive pruning when not improving
// Stockfish-style: lower thresholds = more pruning = fewer nodes
// Formula: base + depth * multiplier, halved when not improving
inline int lmp_threshold(int depth, bool improving) {
    // More aggressive thresholds:
    // Depth 1: 3/4 moves, Depth 2: 5/6 moves, Depth 3: 7/9 moves
    // Depth 4: 9/12 moves, Depth 5: 11/15 moves, Depth 6: 13/18 moves
    int base = 3 + depth * 2;  // Linear scaling instead of quadratic
    return improving ? base : (base * 3 / 2);  // 50% more when not improving
}

// Dynamic Razoring Margin - based on depth
inline int razoring_margin(int depth) {
    return 400 + 150 * depth;
}

// Dynamic Reverse Futility Margin (Static Null Move)
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

// SEE thresholds - more conservative to preserve tactical moves
constexpr int SEE_CAPTURE_THRESHOLD_PER_DEPTH = -25;
constexpr int SEE_QUIET_THRESHOLD_PER_DEPTH = -60;

// History Leaf Pruning threshold (prune moves with very negative history)
constexpr int HISTORY_LEAF_PRUNING_DEPTH = 4;    // Max depth for history pruning
constexpr int HISTORY_LEAF_PRUNING_MARGIN = 8000; // Base margin per depth

// Countermove History Pruning
constexpr int COUNTER_HIST_PRUNING_DEPTH = 3;    // Max depth for countermove history pruning
constexpr int COUNTER_HIST_PRUNING_MARGIN = 4000; // Threshold for pruning (negative history)

// Follow-up History Pruning
constexpr int FOLLOWUP_HIST_PRUNING_DEPTH = 2;   // Max depth for follow-up history pruning
constexpr int FOLLOWUP_HIST_PRUNING_MARGIN = 6000; // Threshold for pruning

// ============================================================================
// Extension Parameters
// ============================================================================

constexpr int MAX_EXTENSIONS = 6;           // Reduced from 6 for better NPS

// Singular extension parameters
constexpr int SINGULAR_DEPTH = 6;            // Minimum depth for singular extensions
constexpr int SINGULAR_MARGIN = 64;          // Base score margin for singularity
constexpr int SINGULAR_TT_DEPTH_PENALTY = 8; // Additional margin per depth difference
constexpr int SINGULAR_IMPROVING_BONUS = 10; // Tighter margin when not improving
constexpr int SINGULAR_DOUBLE_EXT_BASE = 60; // Base threshold for double extension

// Capture extension parameters
constexpr int CAPTURE_EXT_MIN_DEPTH = 6;     // Minimum depth for capture extension
constexpr int CAPTURE_EXT_SEE_THRESHOLD = 0; // SEE threshold for extending captures

// Mate Threat Extension parameters
constexpr int MATE_THREAT_EXT_MIN_DEPTH = 4; // Minimum depth for mate threat extension

// PV Extension parameters
constexpr int PV_EXT_MIN_DEPTH = 5;          // Minimum depth for PV extension

// One Reply Extension - REMOVED (overhead > benefit)
// constexpr int ONE_REPLY_EXT_MIN_DEPTH = 4;

// ============================================================================
// Multi-Cut Parameters
// ============================================================================

constexpr int MULTI_CUT_DEPTH = 10;          // Minimum depth for multi-cut
constexpr int MULTI_CUT_COUNT = 3;           // Number of moves to try
constexpr int MULTI_CUT_REQUIRED = 2;        // Number of cutoffs required

// ============================================================================
// ProbCut Parameters
// ============================================================================

constexpr int PROBCUT_DEPTH = 64;            // Effectively disabled - overhead hurts NPS
constexpr int PROBCUT_MARGIN = 200;          // Score margin above beta

// ============================================================================
// Null Move Parameters
// ============================================================================

constexpr int NULL_MOVE_MIN_DEPTH = 3;       // Minimum depth for null move pruning
constexpr int NULL_MOVE_VERIFY_DEPTH = 16;   // Depth to start verification

// ============================================================================
// Aspiration Window Parameters
// ============================================================================

constexpr int ASPIRATION_INITIAL_DELTA = 18; // Initial aspiration window size
constexpr int ASPIRATION_MIN_DEPTH = 5;      // Minimum depth to use aspiration windows

// ============================================================================
// IIR (Internal Iterative Reductions) Parameters
// ============================================================================

constexpr int IIR_MIN_DEPTH = 4;             // Minimum depth for IIR
constexpr int IIR_REDUCTION = 1;             // Reduction amount
constexpr int IIR_PV_REDUCTION = 1;          // Reduction in PV nodes
constexpr int IIR_CUT_REDUCTION = 2;         // Reduction in cut nodes

// ============================================================================
// Quiescence Search Parameters
// ============================================================================

constexpr int QSEARCH_CHECK_DEPTH = 0;      // Only generate quiet checks deeper into qsearch
constexpr int DELTA_PRUNING_MARGIN = 450;    // Delta pruning margin for better tactics

// ============================================================================
// LMR Tuning Parameters
// ============================================================================

constexpr double LMR_BASE = 0.5;             // Base reduction factor
constexpr double LMR_DIVISOR = 2.25;         // Divisor for log scaling

// ============================================================================
// Extension Control Parameters
// ============================================================================

// Double Extension Control
constexpr int DOUBLE_EXT_LIMIT = 4;          // Reduced from 4 for better NPS
constexpr int MAX_EXTENSION_PLY_RATIO = 2;   // Extension ratio for tactical depth

// Negative Extension (extend when expected fail-high fails)
constexpr int NEG_EXT_THRESHOLD = 100;       // Score threshold below alpha to trigger
constexpr int NEG_EXT_MIN_DEPTH = 6;         // Minimum depth for negative extension

// ============================================================================
// Dynamic SEE Thresholds
// ============================================================================

constexpr int SEE_CAPTURE_IMPROVING_FACTOR = 15;    // More lenient when improving
constexpr int SEE_CAPTURE_NOT_IMPROVING_FACTOR = 30; // Stricter when not improving
constexpr int SEE_QUIET_IMPROVING_FACTOR = 35;       // Quiet SEE when improving
constexpr int SEE_QUIET_NOT_IMPROVING_FACTOR = 60;   // Quiet SEE when not improving

// ============================================================================
// History & LMR Weights
// ============================================================================

constexpr int CONT_HIST_1PLY_WEIGHT = 2;     // Weight for 1-ply continuation history
constexpr int CONT_HIST_2PLY_WEIGHT = 1;     // Weight for 2-ply continuation history
constexpr int CONT_HIST_4PLY_WEIGHT = 1;     // Weight for 4-ply continuation history
constexpr int HISTORY_LMR_DIVISOR = 4000;    // Divisor for history-based LMR adjustment
constexpr int HISTORY_LMR_MAX_ADJ = 3;       // Maximum LMR adjustment from history

} // namespace SearchParams

#endif // SEARCH_CONSTANTS_HPP
