#ifndef NNUE_HPP
#define NNUE_HPP

#include "types.hpp"
#include <cstdint>
#include <string>

class Board;

namespace NNUE {

constexpr int QA = 255;
constexpr int QB = 64;
constexpr int SCALE = 400;

constexpr int FEATURE_COUNT = 768;
constexpr int HIDDEN_SIZE = 512;
constexpr int ACCUMULATOR_SIZE = HIDDEN_SIZE;
constexpr int OUTPUT_INPUT_SIZE = HIDDEN_SIZE * 2;
constexpr int OUTPUT_WEIGHTS_COUNT = OUTPUT_INPUT_SIZE;
constexpr int OUTPUT_BUCKETS = 8;

// GCNNUE05 (GC-RelGeo Dual-Stream CP-HG + Dual-Head V * (1 - DrawGate))
constexpr int RELGEO_FEATURES = 2619;
constexpr int RELGEO_HIDDEN = 128;
constexpr int MLP_INPUT_SIZE = OUTPUT_INPUT_SIZE + RELGEO_HIDDEN; // 1152
constexpr int MLP_HIDDEN = 32;

constexpr size_t EXPECTED_FILE_SIZE_V2 = 789516;
constexpr size_t EXPECTED_FILE_SIZE_V3 = 803880;
constexpr size_t EXPECTED_FILE_SIZE_V4 = 803880;
constexpr size_t EXPECTED_FILE_SIZE_V5 = 1512664;
constexpr size_t EXPECTED_FILE_SIZE = EXPECTED_FILE_SIZE_V4;

int network_version();

inline int output_bucket(int pieceCount) {
    int b = (pieceCount - 1) / 4;
    return (b < 0) ? 0 : (b > 7 ? 7 : b);
}

struct alignas(32) Accumulator {
    alignas(32) int16_t v[2][HIDDEN_SIZE];
};

// Feature indexing matching PROJECT.md interface contract:
// color_rel = c ^ persp
// sq_rel = (persp == WHITE) ? sq : (sq ^ 56)
// idx = color_rel * 384 + (pt - 1) * 64 + sq_rel
inline int feature_index(Color persp, Color c, PieceType pt, Square sq) {
    int color_rel = c ^ persp;
    Square sq_rel = (persp == WHITE) ? sq : Square(sq ^ 56);
    return color_rel * 384 + (pt - 1) * 64 + sq_rel;
}

inline int feature_index(Color persp, Piece pc, Square sq) {
    return feature_index(persp, color_of(pc), type_of(pc), sq);
}

// Weights storage declarations
alignas(32) extern int16_t featureWeights[FEATURE_COUNT][HIDDEN_SIZE];
alignas(32) extern int16_t featureBiases[HIDDEN_SIZE];
alignas(32) extern int16_t outputWeights[OUTPUT_BUCKETS][OUTPUT_INPUT_SIZE];
extern int32_t outputBiases[OUTPUT_BUCKETS];

// GCNNUE05 weights (RelGeo Stream B + Fusion MLP32 + Dual-Head V/DrawGate)
alignas(32) extern int16_t relWeights[RELGEO_FEATURES][RELGEO_HIDDEN];
alignas(32) extern int16_t relBiases[RELGEO_HIDDEN];
alignas(32) extern int8_t  mlp1Weights[MLP_HIDDEN][MLP_INPUT_SIZE];
alignas(32) extern int32_t mlp1Biases[MLP_HIDDEN];
alignas(32) extern int16_t mlpOutVWeights[OUTPUT_BUCKETS][MLP_HIDDEN];
alignas(32) extern int16_t mlpOutDWeights[OUTPUT_BUCKETS][MLP_HIDDEN];
extern int32_t outDBiases[OUTPUT_BUCKETS];

// Extracts canonical STM-oriented sparse RelGeo indices (max 49 active features)
void extract_relgeo_features(const Board& board, int* outIndices, int& outCount);

void init();
bool load_eval_file(const std::string& path);
bool is_loaded();

// Full refresh: resets accumulator to biases and accumulates all pieces on board
void refresh_accumulator(const Board& board, Accumulator& acc);
void refresh_perspective(const Board& board, Accumulator& acc, Color persp);

// Incremental updates:
// 1. Update single perspective
void update_accumulator(Accumulator& acc, Color persp,
                        const int addedFeatures[], int addedCount,
                        const int removedFeatures[], int removedCount);

// 2. Update with arrays for both perspectives
void update_accumulator(const Accumulator& prev, Accumulator& curr,
                        const int addedFeaturesWhite[], int addedCountWhite,
                        const int removedFeaturesWhite[], int removedCountWhite,
                        const int addedFeaturesBlack[], int addedCountBlack,
                        const int removedFeaturesBlack[], int removedCountBlack);

// 3. Update matching PROJECT.md signature
void update_accumulator(const Accumulator& prev, Accumulator& curr,
                        const int addedFeatures[], int addedCount,
                        const int removedFeatures[], int removedCount);

// 4. Piece-based delta helpers
void add_piece(Accumulator& acc, Piece pc, Square sq);
void remove_piece(Accumulator& acc, Piece pc, Square sq);

// Forward evaluation
int evaluate(const Board& board);
int evaluate(const Accumulator& acc, Color sideToMove, int bucket = 7);
int evaluate(const Board& board, const Accumulator& acc);

// Verification helper
bool verify_accumulator(const Board& board, const Accumulator& acc);

} // namespace NNUE

#endif // NNUE_HPP
