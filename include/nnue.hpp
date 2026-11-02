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
constexpr int ACCUMULATOR_SIZE = 256;
constexpr int OUTPUT_WEIGHTS_COUNT = 512;
constexpr size_t EXPECTED_FILE_SIZE = 394764;

struct alignas(32) Accumulator {
    alignas(32) int16_t v[2][256];
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
alignas(32) extern int16_t featureWeights[768][256];
alignas(32) extern int16_t featureBiases[256];
alignas(32) extern int16_t outputWeights[512];
extern int32_t outputBias;

void init();
bool load_eval_file(const std::string& path);
bool is_loaded();

// Full refresh: resets accumulator to biases and accumulates all pieces on board
void refresh_accumulator(const Board& board, Accumulator& acc);

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
int evaluate(const Accumulator& acc, Color sideToMove);
int evaluate(const Board& board, const Accumulator& acc);

// Verification helper
bool verify_accumulator(const Board& board, const Accumulator& acc);

} // namespace NNUE

#endif // NNUE_HPP
