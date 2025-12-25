#ifndef SEARCH_CONSTANTS_HPP
#define SEARCH_CONSTANTS_HPP


namespace SearchParams {

// ============================================================================
// Time Management Constants
// ============================================================================

constexpr int TIME_CHECK_INTERVAL = 2048;     // Check time every N nodes (must be power of 2 - 1)
constexpr double EARLY_STOP_RATIO = 0.6;      // Stop early if elapsed > optimum * ratio
constexpr double PANIC_TIME_EXTENSION = 1.5;  // Extend time by this factor in panic mode
constexpr int SCORE_FLUCTUATION_THRESHOLD = 50;  // Centipawns change to trigger panic

// ============================================================================
// Pruning Parameters
// ============================================================================

// Futility pruning margins per depth
// [PERBAIKAN] Increased margins for less aggressive pruning
constexpr int FutilityMargin[7] = { 0, 200, 400, 600, 800, 1000, 1200 };

// Razoring margins per depth
constexpr int RazorMargin[4] = { 0, 300, 500, 700 };

// Reverse futility pruning (static null move) margins
constexpr int RFPMargin[7] = { 0, 100, 200, 300, 400, 500, 600 };

// Late move pruning thresholds (skip quiet moves after this many tries at low depth)
// [PERBAIKAN] More generous thresholds to search more moves before pruning
constexpr int LMPThreshold[8] = { 0, 6, 10, 16, 24, 34, 46, 60 };

// Maximum depth for specific pruning techniques
constexpr int FUTILITY_MAX_DEPTH = 6;
constexpr int RAZORING_MAX_DEPTH = 3;
constexpr int RFP_MAX_DEPTH = 6;
constexpr int LMP_MAX_DEPTH = 7;
constexpr int SEE_QUIET_MAX_DEPTH = 3;
constexpr int SEE_CAPTURE_MAX_DEPTH = 4;

// SEE thresholds - more conservative to preserve tactical moves
constexpr int SEE_CAPTURE_THRESHOLD_PER_DEPTH = -20;
constexpr int SEE_QUIET_THRESHOLD_PER_DEPTH = -50;

// History Leaf Pruning threshold (prune moves with very negative history)
constexpr int HISTORY_LEAF_PRUNING_DEPTH = 4;    // Max depth for history pruning
constexpr int HISTORY_LEAF_PRUNING_MARGIN = 8000; // Base margin per depth

// Countermove History Pruning
// Prune quiet moves with very poor countermove history scores
constexpr int COUNTER_HIST_PRUNING_DEPTH = 3;    // Max depth for countermove history pruning
constexpr int COUNTER_HIST_PRUNING_MARGIN = 4000; // Threshold for pruning (negative history)

// Follow-up History Pruning
// Prune moves based on 4-ply continuation history pattern
constexpr int FOLLOWUP_HIST_PRUNING_DEPTH = 3;   // Max depth for follow-up history pruning
constexpr int FOLLOWUP_HIST_PRUNING_MARGIN = 5000; // Threshold for pruning

// ============================================================================
// Extension Parameters
// ============================================================================

constexpr int MAX_EXTENSIONS = 16;           // Maximum total extensions in path

// Singular extension parameters
constexpr int SINGULAR_DEPTH = 6;            // Minimum depth for singular extension
constexpr int SINGULAR_MARGIN = 64;          // Base score margin for singularity
constexpr int SINGULAR_TT_DEPTH_PENALTY = 8; // Additional margin per depth difference
constexpr int SINGULAR_IMPROVING_BONUS = 10; // Tighter margin when not improving
constexpr int SINGULAR_DOUBLE_EXT_BASE = 40; // Base threshold for double extension

// Capture extension parameters
constexpr int CAPTURE_EXT_MIN_DEPTH = 6;     // Minimum depth for capture extension
constexpr int CAPTURE_EXT_SEE_THRESHOLD = 200; // SEE threshold for extending captures

// ============================================================================
// Multi-Cut Parameters
// ============================================================================

constexpr int MULTI_CUT_DEPTH = 5;           // Minimum depth for multi-cut
constexpr int MULTI_CUT_COUNT = 3;           // Number of moves to try
constexpr int MULTI_CUT_REQUIRED = 2;        // Number of cutoffs required

// ============================================================================
// ProbCut Parameters
// ============================================================================

constexpr int PROBCUT_DEPTH = 5;             // Minimum depth for ProbCut
constexpr int PROBCUT_MARGIN = 100;          // Score margin above beta

// ============================================================================
// Null Move Parameters
// ============================================================================

constexpr int NULL_MOVE_MIN_DEPTH = 3;       // Minimum depth for null move pruning
constexpr int NULL_MOVE_VERIFY_DEPTH = 12;   // Depth to start verification search

// ============================================================================
// Aspiration Window Parameters
// ============================================================================

constexpr int ASPIRATION_INITIAL_DELTA = 50; // Initial aspiration window size
constexpr int ASPIRATION_MIN_DEPTH = 5;      // Minimum depth to use aspiration windows
constexpr int ASPIRATION_MAX_FAILS = 3;      // Max fails before using full window

// ============================================================================
// IIR (Internal Iterative Reductions) Parameters
// Replaces IID - more efficient depth reduction instead of shallow search
// ============================================================================

constexpr int IIR_MIN_DEPTH = 4;             // Minimum depth for IIR
constexpr int IIR_REDUCTION = 1;             // Depth reduction when no TT move
constexpr int IIR_PV_REDUCTION = 1;          // Reduction in PV nodes
constexpr int IIR_CUT_REDUCTION = 2;         // Reduction in cut nodes (more aggressive)

// ============================================================================
// Quiescence Search Parameters
// ============================================================================

constexpr int QSEARCH_CHECK_DEPTH = 2;       // Depth to search quiet checks in qsearch
constexpr int DELTA_PRUNING_MARGIN = 200;    // Delta pruning margin

// ============================================================================
// LMR Tuning Parameters
// ============================================================================

constexpr double LMR_BASE = 0.50;            // Base reduction factor (more conservative)
constexpr double LMR_DIVISOR = 2.40;         // Divisor for log formula (closer to Stockfish)

// ============================================================================
// Extension Control Parameters
// ============================================================================

// Double/Triple Extension Prevention
constexpr int DOUBLE_EXT_LIMIT = 6;          // Max double extensions allowed
constexpr int MAX_EXTENSION_PLY_RATIO = 2;   // Ply must not exceed depth*ratio after extensions

// Negative Extension (extend when expected fail-high fails)
constexpr int NEG_EXT_THRESHOLD = 100;       // Score threshold below alpha to trigger
constexpr int NEG_EXT_MIN_DEPTH = 6;         // Minimum depth for negative extension

// ============================================================================
// Dynamic SEE Thresholds
// ============================================================================

constexpr int SEE_CAPTURE_IMPROVING_FACTOR = 20;    // More lenient when improving
constexpr int SEE_CAPTURE_NOT_IMPROVING_FACTOR = 40; // Stricter when not improving
constexpr int SEE_QUIET_IMPROVING_FACTOR = 40;       // Quiet SEE when improving
constexpr int SEE_QUIET_NOT_IMPROVING_FACTOR = 70;   // Quiet SEE when not improving

// ============================================================================
// History Pruning Enhancement
// ============================================================================

constexpr int COUNTER_MOVE_HISTORY_BONUS = 2; // Weight for countermove history in LMR

// ============================================================================
// Continuation History Weighting
// These parameters control how much weight is given to different history types
// when scoring moves for move ordering and LMR adjustments
// ============================================================================

constexpr int CONT_HIST_1PLY_WEIGHT = 2;     // Weight for 1-ply continuation history (most relevant)
constexpr int CONT_HIST_2PLY_WEIGHT = 1;     // Weight for 2-ply continuation history
constexpr int CONT_HIST_4PLY_WEIGHT = 1;     // Weight for 4-ply continuation history (less relevant)
constexpr int HISTORY_LMR_DIVISOR = 4000;    // Divisor for history-based LMR adjustment
constexpr int HISTORY_LMR_MAX_ADJ = 3;       // Maximum LMR adjustment from history

} // namespace SearchParams

#endif // SEARCH_CONSTANTS_HPP
