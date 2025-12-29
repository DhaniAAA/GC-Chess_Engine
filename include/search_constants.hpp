#ifndef SEARCH_CONSTANTS_HPP
#define SEARCH_CONSTANTS_HPP

#include <algorithm>  // For std::min, std::max in dynamic functions
namespace SearchParams {

// ============================================================================
// Time Management Constants
// ============================================================================

constexpr int TIME_CHECK_INTERVAL = 2048;     // Check time every N nodes (must be power of 2 - 1)
constexpr double EARLY_STOP_RATIO = 0.6;      // Stop early if elapsed > optimum * ratio
constexpr double PANIC_TIME_EXTENSION = 1.5;  // Extend time by this factor in panic mode
constexpr int SCORE_FLUCTUATION_THRESHOLD = 50;  // Centipawns change to trigger panic

// ============================================================================
// DYNAMIC PARAMETER FUNCTIONS (Stockfish-style)
// These functions compute parameters based on position characteristics
// ============================================================================

// Dynamic Futility Margin - looser when improving, tighter when not
inline int futility_margin(int depth, bool improving) {
    return 150 * depth - (improving ? 50 : 0);
}

// Dynamic LMP Threshold - more aggressive pruning when not improving
inline int lmp_threshold(int depth, bool improving) {
    return (3 + depth * depth) / (improving ? 2 : 1);
}

// Dynamic Null Move Reduction - based on depth and evaluation margin
// Stockfish: R = min((depth + eval - beta) / 200 + 4, depth - 1)
inline int null_move_reduction(int depth, int eval, int beta) {
    int r = 4 + depth / 6; //before dpeth / 3
    // Increase reduction if eval is much better than beta
    if (eval > beta) {
        r += std::min((eval - beta) / 200, 3);
    }
    return std::min(r, depth - 1);
}

// Dynamic Singular Extension Margin - tighter at higher depths
// Stockfish: margin = ttScore - singularExtensionMarginMultiplier * depth
inline int singular_margin(int depth) {
    return 64 + 16 * depth;  // Gets tighter as depth increases
}

// Dynamic Razoring Margin - based on depth
// Tuning: Base 400 with predicted depth logic
inline int razoring_margin(int depth) {
    return 400 + 150 * depth;
}

// Dynamic Reverse Futility Margin (Static Null Move)
// SimpleChessEngine: 120 * depth for depth < 3
// Using 120*depth base with improving adjustment
inline int rfp_margin(int depth, bool improving) {
    return 95 * depth - (improving ? 50 : 0); //before 120 * depth, 20 : 0
}

// Dynamic SEE Threshold for quiet move pruning
// More lenient when improving, stricter when not
inline int see_quiet_threshold(int depth, bool improving) {
    return improving ? (-40 * depth) : (-70 * depth);
}

// Dynamic SEE Threshold for capture pruning
inline int see_capture_threshold(int depth, bool improving) {
    // [FIX] Removed buggy "safety clamp" that disabled SEE pruning for depth > 4
    // The clamp was setting threshold to -1000, which let ALL captures through
    // Now using proper scaling: allow captures that don't lose too much material
    int threshold = improving ? (-60 * depth) : (-100 * depth);

    // Clamp to reasonable bounds: never prune queen sacrifices (-900)
    // but still prune clearly losing captures at any depth
    return std::max(threshold, -850);
}

// Dynamic History Pruning Threshold
// Prune moves with low history scores, threshold depends on depth
inline int history_pruning_threshold(int depth) {
    return -4000 * depth;  // More negative threshold at higher depths
}
constexpr int FUTILITY_MAX_DEPTH = 6;
constexpr int RAZORING_MAX_DEPTH = 3;
constexpr int RFP_MAX_DEPTH = 6;
constexpr int LMP_MAX_DEPTH = 7;
constexpr int SEE_QUIET_MAX_DEPTH = 3;
constexpr int SEE_CAPTURE_MAX_DEPTH = 4; //before 4

// ============================================================================
// Pruning Depths
// ============================================================================

// SEE thresholds - more conservative to preserve tactical moves
constexpr int SEE_CAPTURE_THRESHOLD_PER_DEPTH = -25;
constexpr int SEE_QUIET_THRESHOLD_PER_DEPTH = -60;

// History Leaf Pruning threshold (prune moves with very negative history)
constexpr int HISTORY_LEAF_PRUNING_DEPTH = 4;    // Max depth for history pruning
constexpr int HISTORY_LEAF_PRUNING_MARGIN = 8000; // Base margin per depth

// Countermove History Pruning
// Prune quiet moves with very poor countermove history scores
constexpr int COUNTER_HIST_PRUNING_DEPTH = 3;    // Max depth for countermove history pruning
constexpr int COUNTER_HIST_PRUNING_MARGIN = 4000; // Threshold for pruning (negative history)

// Follow-up History Pruning
// Prune moves based on 4-ply continuation history pattern
constexpr int FOLLOWUP_HIST_PRUNING_DEPTH = 2;   // Max depth for follow-up history pruning (conservative)
constexpr int FOLLOWUP_HIST_PRUNING_MARGIN = 6000; // Threshold for pruning

// ============================================================================
// Extension Parameters
// ============================================================================

constexpr int MAX_EXTENSIONS = 6;           // Increased for better tactical vision

// Singular extension parameters - RE-ENABLED for better move ordering
constexpr int SINGULAR_DEPTH = 6;            // [FIX] Lowered from 8 to catch tactics at shallower depths
constexpr int SINGULAR_MARGIN = 64;          // Base score margin for singularity (tighter)
constexpr int SINGULAR_TT_DEPTH_PENALTY = 8; // Additional margin per depth difference
constexpr int SINGULAR_IMPROVING_BONUS = 10; // Tighter margin when not improving
constexpr int SINGULAR_DOUBLE_EXT_BASE = 60; // Base threshold for double extension (higher = less double ext)

// Capture extension parameters
constexpr int CAPTURE_EXT_MIN_DEPTH = 6;     // Minimum depth for capture extension
constexpr int CAPTURE_EXT_SEE_THRESHOLD = 0; // before 200 SEE threshold for extending captures

// Mate Threat Extension parameters
// When null move search shows opponent can mate us, extend to find defenses
constexpr int MATE_THREAT_EXT_MIN_DEPTH = 4; // Minimum depth for mate threat extension

// PV Extension parameters
// Extend first move in PV nodes at sufficient depth for deeper analysis
constexpr int PV_EXT_MIN_DEPTH = 5;          // Minimum depth for PV extension

// One Reply Extension parameters Disabled
constexpr int ONE_REPLY_EXT_MIN_DEPTH = 4;   // [FIX] Re-enabled - extend forced moves for better tactical depth

// ============================================================================
// Multi-Cut Parameters
// ============================================================================

constexpr int MULTI_CUT_DEPTH = 10;          // RE-ENABLED - minimum depth for multi-cut
constexpr int MULTI_CUT_COUNT = 3;           // Number of moves to try
constexpr int MULTI_CUT_REQUIRED = 2;        // Number of cutoffs required

// ============================================================================
// ProbCut Parameters
// ============================================================================

constexpr int PROBCUT_DEPTH = 64;            // Effectively disabled (was 5) - overhead hurts NPS
constexpr int PROBCUT_MARGIN = 200;          // Score margin above beta

// ============================================================================
// Null Move Parameters
// ============================================================================

constexpr int NULL_MOVE_MIN_DEPTH = 3;       // Minimum depth for null move pruning
constexpr int NULL_MOVE_VERIFY_DEPTH = 16;   // Depth to start verification (was 12)

// ============================================================================
// Aspiration Window Parameters
// ============================================================================

constexpr int ASPIRATION_INITIAL_DELTA = 18; // Initial aspiration window size
constexpr int ASPIRATION_MIN_DEPTH = 5;      // Minimum depth to use aspiration windows

// ============================================================================
// IIR (Internal Iterative Reductions) Parameters
// ============================================================================

constexpr int IIR_MIN_DEPTH = 4;             // Minimum depth for IIR
constexpr int IIR_REDUCTION = 1;             // Reduced from 2 to preserve tactical depth
constexpr int IIR_PV_REDUCTION = 1;          // Reduction in PV nodes
constexpr int IIR_CUT_REDUCTION = 2;         // Reduced from 3 to preserve tactics in cut nodes

// ============================================================================
// Quiescence Search Parameters
// ============================================================================

constexpr int QSEARCH_CHECK_DEPTH = 0;       // [FIX] Set to 0 to enable quiet checks from qsearch start
constexpr int DELTA_PRUNING_MARGIN = 450;    // Delta pruning margin for better tactics

// ============================================================================
// LMR Tuning Parameters
// ============================================================================

// LMR Tuning Parameters - BALANCED for tactics + speed
constexpr double LMR_BASE = 0.5;            // Base reduction factor
constexpr double LMR_DIVISOR = 2.25;         // Divisor for log scaling

// ============================================================================
// Extension Control Parameters
// ============================================================================

// Double Extension Control - balanced for tactics vs NPS
constexpr int DOUBLE_EXT_LIMIT = 4;          // Increased from 3 for better tactics
constexpr int MAX_EXTENSION_PLY_RATIO = 2;   // Restored to 2 for tactical depth in critical lines

// Negative Extension (extend when expected fail-high fails)
constexpr int NEG_EXT_THRESHOLD = 100;       // Score threshold below alpha to trigger
constexpr int NEG_EXT_MIN_DEPTH = 6;         // Minimum depth for negative extension

// ============================================================================
// Dynamic SEE Thresholds
// ============================================================================

// Dynamic SEE Thresholds - balanced for speed+accuracy
constexpr int SEE_CAPTURE_IMPROVING_FACTOR = 15;    // More lenient when improving
constexpr int SEE_CAPTURE_NOT_IMPROVING_FACTOR = 30; // Stricter when not improving
constexpr int SEE_QUIET_IMPROVING_FACTOR = 35;       // Quiet SEE when improving
constexpr int SEE_QUIET_NOT_IMPROVING_FACTOR = 60;   // Quiet SEE when not improving

// ============================================================================
// History & LMR Weights
// ============================================================================
constexpr int COUNTER_MOVE_HISTORY_BONUS = 2; // Weight for countermove history in LMR
constexpr int CONT_HIST_1PLY_WEIGHT = 2;     // Weight for 1-ply continuation history (most relevant)
constexpr int CONT_HIST_2PLY_WEIGHT = 1;     // Weight for 2-ply continuation history
constexpr int CONT_HIST_4PLY_WEIGHT = 1;     // Weight for 4-ply continuation history (less relevant)
constexpr int HISTORY_LMR_DIVISOR = 4000;    // Divisor for history-based LMR adjustment
constexpr int HISTORY_LMR_MAX_ADJ = 3;       // Maximum LMR adjustment from history

} // namespace SearchParams

#endif // SEARCH_CONSTANTS_HPP
