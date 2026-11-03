#include "nnue.hpp"
#include "board.hpp"
#include "bitboard.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace NNUE {

alignas(32) int16_t featureWeights[FEATURE_COUNT][HIDDEN_SIZE];
alignas(32) int16_t featureBiases[HIDDEN_SIZE];
alignas(32) int16_t outputWeights[OUTPUT_INPUT_SIZE];
int32_t outputBias = 0;

static bool networkLoaded = false;

bool is_loaded() {
    return networkLoaded;
}

void init() {
    networkLoaded = false;
    std::memset(featureWeights, 0, sizeof(featureWeights));
    std::memset(featureBiases, 0, sizeof(featureBiases));
    std::memset(outputWeights, 0, sizeof(outputWeights));
    outputBias = 0;
}

bool load_eval_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "info string NNUE file not found or unreadable: " << path << std::endl;
        networkLoaded = false;
        return false;
    }

    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE)) {
        std::cerr << "info string NNUE file size mismatch: expected " << EXPECTED_FILE_SIZE
                  << " bytes, got " << fileSize << " bytes" << std::endl;
        networkLoaded = false;
        return false;
    }

    char magic[8];
    if (!file.read(magic, 8)) {
        std::cerr << "info string NNUE failed reading magic header from: " << path << std::endl;
        networkLoaded = false;
        return false;
    }

    if (std::memcmp(magic, "GCNNUE02", 8) != 0) {
        std::cerr << "info string NNUE invalid magic header: expected 'GCNNUE02'" << std::endl;
        networkLoaded = false;
        return false;
    }

    if (!file.read(reinterpret_cast<char*>(featureWeights), sizeof(featureWeights))) {
        std::cerr << "info string NNUE failed reading feature weights" << std::endl;
        networkLoaded = false;
        return false;
    }

    if (!file.read(reinterpret_cast<char*>(featureBiases), sizeof(featureBiases))) {
        std::cerr << "info string NNUE failed reading feature biases" << std::endl;
        networkLoaded = false;
        return false;
    }

    if (!file.read(reinterpret_cast<char*>(outputWeights), sizeof(outputWeights))) {
        std::cerr << "info string NNUE failed reading output weights" << std::endl;
        networkLoaded = false;
        return false;
    }

    if (!file.read(reinterpret_cast<char*>(&outputBias), sizeof(outputBias))) {
        std::cerr << "info string NNUE failed reading output bias" << std::endl;
        networkLoaded = false;
        return false;
    }

    networkLoaded = true;
    std::cout << "info string NNUE loaded successfully: " << path << std::endl;
    return true;
}

void refresh_accumulator(const Board& board, Accumulator& acc) {
    std::memcpy(acc.v[WHITE], featureBiases, sizeof(featureBiases));
    std::memcpy(acc.v[BLACK], featureBiases, sizeof(featureBiases));

    Bitboard occ = board.pieces();
    while (occ) {
        Square sq = pop_lsb(occ);
        Piece pc = board.piece_on(sq);
        Color c = color_of(pc);
        PieceType pt = type_of(pc);
        if (pt == NO_PIECE_TYPE) continue;

        int idxW = feature_index(WHITE, c, pt, sq);
        int idxB = feature_index(BLACK, c, pt, sq);

#if defined(__AVX2__)
        for (int i = 0; i < HIDDEN_SIZE; i += 16) {
            __m256i aW = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&acc.v[WHITE][i]));
            __m256i wW = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&featureWeights[idxW][i]));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(&acc.v[WHITE][i]), _mm256_add_epi16(aW, wW));

            __m256i aB = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&acc.v[BLACK][i]));
            __m256i wB = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&featureWeights[idxB][i]));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(&acc.v[BLACK][i]), _mm256_add_epi16(aB, wB));
        }
#else
        for (int i = 0; i < HIDDEN_SIZE; ++i) {
            acc.v[WHITE][i] += featureWeights[idxW][i];
            acc.v[BLACK][i] += featureWeights[idxB][i];
        }
#endif
    }
}

static inline void update_single_perspective(int16_t* acc,
                                             const int addedFeatures[], int addedCount,
                                             const int removedFeatures[], int removedCount) {
#if defined(__AVX2__)
    for (int i = 0; i < HIDDEN_SIZE; i += 16) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&acc[i]));
        for (int r = 0; r < removedCount; ++r) {
            __m256i rw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&featureWeights[removedFeatures[r]][i]));
            v = _mm256_sub_epi16(v, rw);
        }
        for (int a = 0; a < addedCount; ++a) {
            __m256i aw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&featureWeights[addedFeatures[a]][i]));
            v = _mm256_add_epi16(v, aw);
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&acc[i]), v);
    }
#else
    for (int i = 0; i < HIDDEN_SIZE; ++i) {
        int32_t val = acc[i];
        for (int r = 0; r < removedCount; ++r) {
            val -= featureWeights[removedFeatures[r]][i];
        }
        for (int a = 0; a < addedCount; ++a) {
            val += featureWeights[addedFeatures[a]][i];
        }
        acc[i] = static_cast<int16_t>(val);
    }
#endif
}

void update_accumulator(Accumulator& acc, Color persp,
                        const int addedFeatures[], int addedCount,
                        const int removedFeatures[], int removedCount) {
    update_single_perspective(acc.v[persp], addedFeatures, addedCount, removedFeatures, removedCount);
}

void update_accumulator(const Accumulator& prev, Accumulator& curr,
                        const int addedFeaturesWhite[], int addedCountWhite,
                        const int removedFeaturesWhite[], int removedCountWhite,
                        const int addedFeaturesBlack[], int addedCountBlack,
                        const int removedFeaturesBlack[], int removedCountBlack) {
    if (&curr != &prev) {
        std::memcpy(&curr, &prev, sizeof(Accumulator));
    }
    update_single_perspective(curr.v[WHITE], addedFeaturesWhite, addedCountWhite, removedFeaturesWhite, removedCountWhite);
    update_single_perspective(curr.v[BLACK], addedFeaturesBlack, addedCountBlack, removedFeaturesBlack, removedCountBlack);
}

void update_accumulator(const Accumulator& prev, Accumulator& curr,
                        const int addedFeatures[], int addedCount,
                        const int removedFeatures[], int removedCount) {
    if (&curr != &prev) {
        std::memcpy(&curr, &prev, sizeof(Accumulator));
    }
    if (addedCount > 0 && addedCount % 2 == 0 && removedCount % 2 == 0) {
        int halfAdd = addedCount / 2;
        int halfRem = removedCount / 2;
        update_single_perspective(curr.v[WHITE], addedFeatures, halfAdd, removedFeatures, halfRem);
        update_single_perspective(curr.v[BLACK], addedFeatures + halfAdd, halfAdd, removedFeatures + halfRem, halfRem);
    } else {
        update_single_perspective(curr.v[WHITE], addedFeatures, addedCount, removedFeatures, removedCount);
        update_single_perspective(curr.v[BLACK], addedFeatures, addedCount, removedFeatures, removedCount);
    }
}

void add_piece(Accumulator& acc, Piece pc, Square sq) {
    Color c = color_of(pc);
    PieceType pt = type_of(pc);
    if (pt == NO_PIECE_TYPE) return;
    int idxW = feature_index(WHITE, c, pt, sq);
    int idxB = feature_index(BLACK, c, pt, sq);
    int addW[1] = { idxW };
    int addB[1] = { idxB };
    update_single_perspective(acc.v[WHITE], addW, 1, nullptr, 0);
    update_single_perspective(acc.v[BLACK], addB, 1, nullptr, 0);
}

void remove_piece(Accumulator& acc, Piece pc, Square sq) {
    Color c = color_of(pc);
    PieceType pt = type_of(pc);
    if (pt == NO_PIECE_TYPE) return;
    int idxW = feature_index(WHITE, c, pt, sq);
    int idxB = feature_index(BLACK, c, pt, sq);
    int remW[1] = { idxW };
    int remB[1] = { idxB };
    update_single_perspective(acc.v[WHITE], nullptr, 0, remW, 1);
    update_single_perspective(acc.v[BLACK], nullptr, 0, remB, 1);
}

#if defined(__AVX2__)
static inline int forward_avx2(const int16_t* stmAcc, const int16_t* nstmAcc,
                               const int16_t* stmWeights, const int16_t* nstmWeights,
                               int32_t bias) {
    __m256i zero = _mm256_setzero_si256();
    __m256i qa   = _mm256_set1_epi16(QA);
    __m256i sum  = _mm256_setzero_si256();

    for (int i = 0; i < HIDDEN_SIZE; i += 16) {
        // STM perspective
        __m256i acc_stm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmAcc[i]));
        __m256i clamped_stm = _mm256_min_epi16(_mm256_max_epi16(acc_stm, zero), qa);
        __m256i w_stm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmWeights[i]));
        __m256i prod_stm = _mm256_mullo_epi16(clamped_stm, w_stm);
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(clamped_stm, prod_stm));

        // NSTM perspective
        __m256i acc_nstm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i]));
        __m256i clamped_nstm = _mm256_min_epi16(_mm256_max_epi16(acc_nstm, zero), qa);
        __m256i w_nstm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmWeights[i]));
        __m256i prod_nstm = _mm256_mullo_epi16(clamped_nstm, w_nstm);
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(clamped_nstm, prod_nstm));
    }

    // Horizontal sum of 8x 32-bit integers in sum
    __m128i lo = _mm256_castsi256_si128(sum);
    __m128i hi = _mm256_extracti128_si256(sum, 1);
    __m128i hsum = _mm_add_epi32(lo, hi);
    hsum = _mm_hadd_epi32(hsum, hsum);
    hsum = _mm_hadd_epi32(hsum, hsum);
    int32_t raw = _mm_cvtsi128_si32(hsum);

    return static_cast<int>(((raw / QA + bias) * SCALE) / (QA * QB));
}
#endif

static inline int forward_scalar(const int16_t* stmAcc, const int16_t* nstmAcc,
                                 const int16_t* stmWeights, const int16_t* nstmWeights,
                                 int32_t bias) {
    int32_t raw = 0;
    for (int i = 0; i < HIDDEN_SIZE; ++i) {
        int32_t c_stm = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        raw += c_stm * c_stm * stmWeights[i];

        int32_t c_nstm = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        raw += c_nstm * c_nstm * nstmWeights[i];
    }
    return static_cast<int>(((raw / QA + bias) * SCALE) / (QA * QB));
}

int evaluate(const Accumulator& acc, Color sideToMove) {
    if (!networkLoaded) return 0;

    const int16_t* stmAcc  = acc.v[sideToMove];
    const int16_t* nstmAcc = acc.v[~sideToMove];
    const int16_t* stmWeights  = &outputWeights[0];
    const int16_t* nstmWeights = &outputWeights[HIDDEN_SIZE];

#if defined(__AVX2__)
    return forward_avx2(stmAcc, nstmAcc, stmWeights, nstmWeights, outputBias);
#else
    return forward_scalar(stmAcc, nstmAcc, stmWeights, nstmWeights, outputBias);
#endif
}

int evaluate(const Board& board) {
    if (!networkLoaded) return 0;
    Accumulator acc;
    refresh_accumulator(board, acc);
    return evaluate(acc, board.side_to_move());
}

int evaluate(const Board& board, const Accumulator& acc) {
    return evaluate(acc, board.side_to_move());
}

bool verify_accumulator(const Board& board, const Accumulator& acc) {
    Accumulator ref;
    refresh_accumulator(board, ref);
    return std::memcmp(&acc, &ref, sizeof(Accumulator)) == 0;
}

} // namespace NNUE
