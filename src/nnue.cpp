#include "nnue.hpp"
#include "board.hpp"
#include "bitboard.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace NNUE {

alignas(32) int16_t featureWeights[FEATURE_COUNT][HIDDEN_SIZE];
alignas(32) int16_t featureBiases[HIDDEN_SIZE];
alignas(32) int16_t outputWeights[OUTPUT_BUCKETS][OUTPUT_INPUT_SIZE];
int32_t outputBiases[OUTPUT_BUCKETS];

alignas(32) int16_t relWeights[RELGEO_FEATURES][RELGEO_HIDDEN];
alignas(32) int16_t relBiases[RELGEO_HIDDEN];
alignas(32) int8_t  mlp1Weights[MLP_HIDDEN][MLP_INPUT_SIZE];
alignas(32) int32_t mlp1Biases[MLP_HIDDEN];
alignas(32) int16_t mlpOutVWeights[OUTPUT_BUCKETS][MLP_HIDDEN];
alignas(32) int16_t mlpOutDWeights[OUTPUT_BUCKETS][MLP_HIDDEN];
int32_t outDBiases[OUTPUT_BUCKETS];

static bool networkLoaded = false;
static int networkVersion = 4;

bool is_loaded() {
    return networkLoaded;
}

int network_version() {
    return networkVersion;
}

void init() {
    networkLoaded = false;
    networkVersion = 4;
    std::memset(featureWeights, 0, sizeof(featureWeights));
    std::memset(featureBiases, 0, sizeof(featureBiases));
    std::memset(outputWeights, 0, sizeof(outputWeights));
    std::memset(outputBiases, 0, sizeof(outputBiases));
    std::memset(relWeights, 0, sizeof(relWeights));
    std::memset(relBiases, 0, sizeof(relBiases));
    std::memset(mlp1Weights, 0, sizeof(mlp1Weights));
    std::memset(mlp1Biases, 0, sizeof(mlp1Biases));
    std::memset(mlpOutVWeights, 0, sizeof(mlpOutVWeights));
    std::memset(mlpOutDWeights, 0, sizeof(mlpOutDWeights));
    std::memset(outDBiases, 0, sizeof(outDBiases));
}

bool load_eval_file(const std::string& path) {
    std::string resolvedPath = path;
    std::ifstream file(resolvedPath, std::ios::binary);
    if (!file.is_open()) {
        const std::string fallbacks[] = {
            "../" + path,
            "output/" + path,
            "../output/" + path
        };
        for (const auto& candidate : fallbacks) {
            file.clear();
            file.open(candidate, std::ios::binary);
            if (file.is_open()) {
                resolvedPath = candidate;
                break;
            }
        }
    }
    if (!file.is_open()) {
        std::cerr << "info string NNUE file not found or unreadable: " << path << std::endl;
        networkLoaded = false;
        return false;
    }

    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE_V5) &&
        fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE_V4) &&
        fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE_V3) &&
        fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE_V2)) {
        std::cerr << "info string NNUE file size mismatch: expected " << EXPECTED_FILE_SIZE_V5
                  << " (v5), " << EXPECTED_FILE_SIZE_V4 << " (v3/v4), or " << EXPECTED_FILE_SIZE_V2
                  << " (v2) bytes, got " << fileSize << " bytes" << std::endl;
        networkLoaded = false;
        return false;
    }

    char magic[8];
    if (!file.read(magic, 8)) {
        std::cerr << "info string NNUE failed reading magic header from: " << path << std::endl;
        networkLoaded = false;
        return false;
    }

    bool is_v5 = (std::memcmp(magic, "GCNNUE05", 8) == 0);
    bool is_v4 = (std::memcmp(magic, "GCNNUE04", 8) == 0);
    bool is_v3 = (std::memcmp(magic, "GCNNUE03", 8) == 0);
    bool is_v2 = (std::memcmp(magic, "GCNNUE02", 8) == 0);

    if (!is_v5 && !is_v4 && !is_v3 && !is_v2) {
        std::cerr << "info string NNUE invalid magic header: expected 'GCNNUE05', 'GCNNUE04', 'GCNNUE03', or 'GCNNUE02'" << std::endl;
        networkLoaded = false;
        return false;
    }

    if (is_v5) {
        if (fileSize != static_cast<std::streampos>(EXPECTED_FILE_SIZE_V5)) {
            std::cerr << "info string GCNNUE05 file size mismatch: got " << fileSize << " bytes" << std::endl;
            networkLoaded = false;
            return false;
        }
        int32_t meta[4] = {0, 0, 0, 0};
        if (!file.read(reinterpret_cast<char*>(meta), sizeof(meta)) ||
            meta[0] != QA || meta[1] != QB || meta[2] != RELGEO_HIDDEN || meta[3] != MLP_HIDDEN) {
            std::cerr << "info string GCNNUE05 metadata mismatch" << std::endl;
            networkLoaded = false;
            return false;
        }
        if (!file.read(reinterpret_cast<char*>(featureWeights), sizeof(featureWeights)) ||
            !file.read(reinterpret_cast<char*>(featureBiases), sizeof(featureBiases)) ||
            !file.read(reinterpret_cast<char*>(relWeights), sizeof(relWeights)) ||
            !file.read(reinterpret_cast<char*>(relBiases), sizeof(relBiases)) ||
            !file.read(reinterpret_cast<char*>(mlp1Weights), sizeof(mlp1Weights)) ||
            !file.read(reinterpret_cast<char*>(mlp1Biases), sizeof(mlp1Biases)) ||
            !file.read(reinterpret_cast<char*>(outputWeights), sizeof(outputWeights)) ||
            !file.read(reinterpret_cast<char*>(mlpOutVWeights), sizeof(mlpOutVWeights)) ||
            !file.read(reinterpret_cast<char*>(outputBiases), sizeof(outputBiases)) ||
            !file.read(reinterpret_cast<char*>(mlpOutDWeights), sizeof(mlpOutDWeights)) ||
            !file.read(reinterpret_cast<char*>(outDBiases), sizeof(outDBiases))) {
            std::cerr << "info string GCNNUE05 failed reading weights payload" << std::endl;
            networkLoaded = false;
            return false;
        }
        networkVersion = 5;
        networkLoaded = true;
        std::cout << "info string NNUE loaded successfully: " << resolvedPath
                  << " (GCNNUE05 GC-RelGeo Dual-Stream Pure)" << std::endl;
        return true;
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

    if (is_v4 || is_v3) {
        if (!file.read(reinterpret_cast<char*>(outputWeights), sizeof(outputWeights))) {
            std::cerr << "info string NNUE failed reading output weights (v3/v4)" << std::endl;
            networkLoaded = false;
            return false;
        }
        if (!file.read(reinterpret_cast<char*>(outputBiases), sizeof(outputBiases))) {
            std::cerr << "info string NNUE failed reading output biases (v3/v4)" << std::endl;
            networkLoaded = false;
            return false;
        }
    } else {
        // v2 format: single 1024-weight layer and 1 bias -> broadcast across all 8 buckets
        int16_t singleOutWeights[OUTPUT_INPUT_SIZE];
        int32_t singleOutBias = 0;
        if (!file.read(reinterpret_cast<char*>(singleOutWeights), sizeof(singleOutWeights))) {
            std::cerr << "info string NNUE failed reading output weights (v2)" << std::endl;
            networkLoaded = false;
            return false;
        }
        if (!file.read(reinterpret_cast<char*>(&singleOutBias), sizeof(singleOutBias))) {
            std::cerr << "info string NNUE failed reading output bias (v2)" << std::endl;
            networkLoaded = false;
            return false;
        }
        for (int b = 0; b < OUTPUT_BUCKETS; ++b) {
            std::memcpy(outputWeights[b], singleOutWeights, sizeof(singleOutWeights));
            outputBiases[b] = singleOutBias;
        }
    }

    networkVersion = is_v4 ? 4 : (is_v3 ? 3 : 2);
    networkLoaded = true;
    const char* archDesc = is_v4 ? "GCNNUE04 Unified CP-HG"
                         : (is_v3 ? "GCNNUE03 8-bucket" : "GCNNUE02 single-bucket");
    std::cout << "info string NNUE loaded successfully: " << resolvedPath
              << " (" << archDesc << ")" << std::endl;
    return true;
}

void refresh_perspective(const Board& board, Accumulator& acc, Color persp) {
    std::memcpy(acc.v[persp], featureBiases, sizeof(featureBiases));

    Bitboard occ = board.pieces();
    while (occ) {
        Square sq = pop_lsb(occ);
        Piece pc = board.piece_on(sq);
        Color c = color_of(pc);
        PieceType pt = type_of(pc);
        if (pt == NO_PIECE_TYPE) continue;

        int idx = feature_index(persp, c, pt, sq);

#if defined(__AVX2__)
        for (int i = 0; i < HIDDEN_SIZE; i += 16) {
            __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&acc.v[persp][i]));
            __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&featureWeights[idx][i]));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(&acc.v[persp][i]), _mm256_add_epi16(a, w));
        }
#else
        for (int i = 0; i < HIDDEN_SIZE; ++i) {
            acc.v[persp][i] += featureWeights[idx][i];
        }
#endif
    }
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
        __m256i c_stm   = _mm256_min_epi16(_mm256_max_epi16(acc_stm, zero), qa);
        __m256i w_stm   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmWeights[i]));

        __m256i c_stm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_stm));
        __m256i c_stm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_stm, 1));
        __m256i w_stm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_stm));
        __m256i w_stm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_stm, 1));

        __m256i c2_stm_lo = _mm256_mullo_epi32(c_stm_lo, c_stm_lo);
        __m256i c2_stm_hi = _mm256_mullo_epi32(c_stm_hi, c_stm_hi);

        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(c2_stm_lo, w_stm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(c2_stm_hi, w_stm_hi));

        // NSTM perspective
        __m256i acc_nstm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i]));
        __m256i c_nstm   = _mm256_min_epi16(_mm256_max_epi16(acc_nstm, zero), qa);
        __m256i w_nstm   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmWeights[i]));

        __m256i c_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_nstm));
        __m256i c_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_nstm, 1));
        __m256i w_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_nstm));
        __m256i w_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_nstm, 1));

        __m256i c2_nstm_lo = _mm256_mullo_epi32(c_nstm_lo, c_nstm_lo);
        __m256i c2_nstm_hi = _mm256_mullo_epi32(c_nstm_hi, c_nstm_hi);

        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(c2_nstm_lo, w_nstm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(c2_nstm_hi, w_nstm_hi));
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

// GCNNUE04 Unified CP-HG raw dot-product (3-Way Cross-Perspective Gating)
static inline int32_t forward_raw_avx2_v4(const int16_t* stmAcc, const int16_t* nstmAcc,
                                          const int16_t* stmWeights, const int16_t* nstmWeights) {
    __m256i zero = _mm256_setzero_si256();
    __m256i qa   = _mm256_set1_epi16(QA);
    __m256i sum  = _mm256_setzero_si256();

    // Zone 1 (0..255): Pure Perspective SCReLU (s_i^2 and n_i^2)
    for (int i = 0; i < 256; i += 16) {
        __m256i acc_stm  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmAcc[i]));
        __m256i c_stm    = _mm256_min_epi16(_mm256_max_epi16(acc_stm, zero), qa);
        __m256i w_stm    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmWeights[i]));

        __m256i c_stm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_stm));
        __m256i c_stm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_stm, 1));
        __m256i w_stm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_stm));
        __m256i w_stm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_stm, 1));

        __m256i act_stm_lo = _mm256_mullo_epi32(c_stm_lo, c_stm_lo);
        __m256i act_stm_hi = _mm256_mullo_epi32(c_stm_hi, c_stm_hi);

        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(act_stm_lo, w_stm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(act_stm_hi, w_stm_hi));

        __m256i acc_nstm  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i]));
        __m256i c_nstm    = _mm256_min_epi16(_mm256_max_epi16(acc_nstm, zero), qa);
        __m256i w_nstm    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmWeights[i]));

        __m256i c_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_nstm));
        __m256i c_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_nstm, 1));
        __m256i w_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_nstm));
        __m256i w_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_nstm, 1));

        __m256i act_nstm_lo = _mm256_mullo_epi32(c_nstm_lo, c_nstm_lo);
        __m256i act_nstm_hi = _mm256_mullo_epi32(c_nstm_hi, c_nstm_hi);

        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(act_nstm_lo, w_nstm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(act_nstm_hi, w_nstm_hi));
    }

    // Zone 2 (256..383): Direct Hadamard Clash (s_i * n_i) & Squared Dominance ((s_i - n_i)^2)
    for (int i = 256; i < 384; i += 16) {
        __m256i acc_stm  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmAcc[i]));
        __m256i c_stm    = _mm256_min_epi16(_mm256_max_epi16(acc_stm, zero), qa);
        __m256i w_stm    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmWeights[i]));

        __m256i acc_nstm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i]));
        __m256i c_nstm   = _mm256_min_epi16(_mm256_max_epi16(acc_nstm, zero), qa);
        __m256i w_nstm   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmWeights[i]));

        __m256i c_stm_lo  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_stm));
        __m256i c_stm_hi  = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_stm, 1));
        __m256i c_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_nstm));
        __m256i c_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_nstm, 1));

        __m256i w_stm_lo  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_stm));
        __m256i w_stm_hi  = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_stm, 1));
        __m256i w_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_nstm));
        __m256i w_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_nstm, 1));

        // STM slot: s_i * n_i
        __m256i clash_lo = _mm256_mullo_epi32(c_stm_lo, c_nstm_lo);
        __m256i clash_hi = _mm256_mullo_epi32(c_stm_hi, c_nstm_hi);
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(clash_lo, w_stm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(clash_hi, w_stm_hi));

        // NSTM slot: (s_i - n_i)^2
        __m256i diff_lo  = _mm256_sub_epi32(c_stm_lo, c_nstm_lo);
        __m256i diff_hi  = _mm256_sub_epi32(c_stm_hi, c_nstm_hi);
        __m256i dom_lo   = _mm256_mullo_epi32(diff_lo, diff_lo);
        __m256i dom_hi   = _mm256_mullo_epi32(diff_hi, diff_hi);
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(dom_lo, w_nstm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(dom_hi, w_nstm_hi));
    }

    // Zone 3 (384..511): Neighbor-Shuffled Cross-Subspace Gating (s_i * n_{i^1} and n_i * s_{i^1})
    for (int i = 384; i < 512; i += 16) {
        __m256i acc_stm  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmAcc[i]));
        __m256i c_stm    = _mm256_min_epi16(_mm256_max_epi16(acc_stm, zero), qa);
        __m256i w_stm    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&stmWeights[i]));

        __m256i acc_nstm = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i]));
        __m256i c_nstm   = _mm256_min_epi16(_mm256_max_epi16(acc_nstm, zero), qa);
        __m256i w_nstm   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&nstmWeights[i]));

        __m256i c_stm_lo  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_stm));
        __m256i c_stm_hi  = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_stm, 1));
        __m256i c_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(c_nstm));
        __m256i c_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(c_nstm, 1));

        __m256i w_stm_lo  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_stm));
        __m256i w_stm_hi  = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_stm, 1));
        __m256i w_nstm_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w_nstm));
        __m256i w_nstm_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w_nstm, 1));

        // Swap adjacent pairs (2k <-> 2k+1) using 0xB1 = _MM_SHUFFLE(2, 3, 0, 1)
        __m256i n_shuf_lo = _mm256_shuffle_epi32(c_nstm_lo, 0xB1);
        __m256i n_shuf_hi = _mm256_shuffle_epi32(c_nstm_hi, 0xB1);
        __m256i s_shuf_lo = _mm256_shuffle_epi32(c_stm_lo, 0xB1);
        __m256i s_shuf_hi = _mm256_shuffle_epi32(c_stm_hi, 0xB1);

        __m256i x_stm_lo  = _mm256_mullo_epi32(c_stm_lo, n_shuf_lo);
        __m256i x_stm_hi  = _mm256_mullo_epi32(c_stm_hi, n_shuf_hi);
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(x_stm_lo, w_stm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(x_stm_hi, w_stm_hi));

        __m256i x_nstm_lo = _mm256_mullo_epi32(c_nstm_lo, s_shuf_lo);
        __m256i x_nstm_hi = _mm256_mullo_epi32(c_nstm_hi, s_shuf_hi);
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(x_nstm_lo, w_nstm_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(x_nstm_hi, w_nstm_hi));
    }

    __m128i lo = _mm256_castsi256_si128(sum);
    __m128i hi = _mm256_extracti128_si256(sum, 1);
    __m128i hsum = _mm_add_epi32(lo, hi);
    hsum = _mm_hadd_epi32(hsum, hsum);
    hsum = _mm_hadd_epi32(hsum, hsum);
    return _mm_cvtsi128_si32(hsum);
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

static inline int32_t forward_raw_scalar_v4(const int16_t* stmAcc, const int16_t* nstmAcc,
                                            const int16_t* stmWeights, const int16_t* nstmWeights) {
    int32_t raw = 0;
    // Zone 1 (0..255)
    for (int i = 0; i < 256; ++i) {
        int32_t s = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        raw += (s * s) * stmWeights[i];
        raw += (n * n) * nstmWeights[i];
    }
    // Zone 2 (256..383)
    for (int i = 256; i < 384; ++i) {
        int32_t s = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        int32_t diff = s - n;
        raw += (s * n) * stmWeights[i];
        raw += (diff * diff) * nstmWeights[i];
    }
    // Zone 3 (384..511)
    for (int i = 384; i < 512; ++i) {
        int32_t s = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        int32_t s_nb = std::clamp(static_cast<int32_t>(stmAcc[i ^ 1]), 0, QA);
        int32_t n_nb = std::clamp(static_cast<int32_t>(nstmAcc[i ^ 1]), 0, QA);
        raw += (s * n_nb) * stmWeights[i];
        raw += (n * s_nb) * nstmWeights[i];
    }
    return raw;
}

static inline int32_t forward_raw_v4_bucket(const int16_t* stmAcc, const int16_t* nstmAcc, int bucket) {
    const int16_t* stmWeights  = &outputWeights[bucket][0];
    const int16_t* nstmWeights = &outputWeights[bucket][HIDDEN_SIZE];
#if defined(__AVX2__)
    return forward_raw_avx2_v4(stmAcc, nstmAcc, stmWeights, nstmWeights);
#else
    return forward_raw_scalar_v4(stmAcc, nstmAcc, stmWeights, nstmWeights);
#endif
}

void extract_relgeo_features(const Board& board, int* outIndices, int& outCount) {
    Color stm = board.side_to_move();
    Color nstm = ~stm;
    int flipV = (stm == BLACK) ? 56 : 0;
    Square wkPersp = Square(int(board.king_square(stm)) ^ flipV);
    int mirrorH = (file_of(wkPersp) < FILE_E) ? 7 : 0;

    auto canon = [&](Square sq) -> Square {
        return Square(int(sq) ^ flipV ^ mirrorH);
    };
    auto is_canon_light = [](Square sq) -> bool {
        return ((int(file_of(sq)) + int(rank_of(sq))) & 1) != 0;
    };

    Square okSq = canon(board.king_square(stm));
    Square ekSq = canon(board.king_square(nstm));
    int okF = int(file_of(okSq));
    int okR = int(rank_of(okSq));
    int ekF = int(file_of(ekSq));
    int ekR = int(rank_of(ekSq));

    int n = 0;
    // 1. King-King Relative Vector [0..224]
    outIndices[n++] = (ekR - okR + 7) * 15 + (ekF - okF + 7);
    // 2. Canonical STM King [225..288] & Enemy King [289..352]
    outIndices[n++] = 225 + int(okSq);
    outIndices[n++] = 289 + int(ekSq);

    // 3. Piece-to-EnemyKing Relative Offset [353..2602]
    Bitboard nonKings = board.pieces() & ~board.pieces(KING);
    while (nonKings) {
        Square sq = pop_lsb(nonKings);
        Piece pc = board.piece_on(sq);
        int cRel = (color_of(pc) == stm) ? 0 : 1;
        int ptIdx = int(type_of(pc)) - 1;
        Square psq = canon(sq);
        int dx = int(file_of(psq)) - ekF;
        int dy = int(rank_of(psq)) - ekR;
        outIndices[n++] = 353 + (cRel * 5 + ptIdx) * 225 + (dy + 7) * 15 + (dx + 7);
    }

    // 4. Bishop-Corner Parity & Pure OCB [2603..2606]
    bool hasLightB = false;
    bool hasDarkB = false;
    bool stmBLight = false;
    int stmBCount = 0;
    Bitboard stmB = board.pieces(stm, BISHOP);
    while (stmB) {
        Square bsq = canon(pop_lsb(stmB));
        bool light = is_canon_light(bsq);
        if (light) hasLightB = true;
        else hasDarkB = true;
        stmBLight = light;
        ++stmBCount;
    }
    if (hasLightB) outIndices[n++] = 2603;
    if (hasDarkB)  outIndices[n++] = 2604;
    if (is_canon_light(ekSq)) outIndices[n++] = 2605;

    Bitboard nstmB = board.pieces(nstm, BISHOP);
    if (board.pieces(KNIGHT) == 0 && board.pieces(ROOK) == 0 && board.pieces(QUEEN) == 0 &&
        stmBCount == 1 && popcount(nstmB) == 1) {
        bool nstmBLight = is_canon_light(canon(lsb(nstmB)));
        if (stmBLight != nstmBLight) {
            outIndices[n++] = 2606;
        }
    }

    // 5. Halfmove Clock Thermometer [2607..2618]
    static constexpr int HMC_THRESH[12] = {8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96};
    int hmc = board.halfmove_clock();
    if (hmc >= 8) {
        for (int k = 0; k < 12 && hmc >= HMC_THRESH[k]; ++k) {
            outIndices[n++] = 2607 + k;
        }
    }

    outCount = n;
}

static inline int32_t dot_i16_i8(const int16_t* a, const int8_t* w, int len) {
#if defined(__AVX2__)
    __m256i sum = _mm256_setzero_si256();
    for (int i = 0; i < len; i += 16) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a[i]));
        __m128i vw8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&w[i]));
        __m256i vw = _mm256_cvtepi8_epi16(vw8);
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(va, vw));
    }
    __m128i lo = _mm256_castsi256_si128(sum);
    __m128i hi = _mm256_extracti128_si256(sum, 1);
    __m128i hsum = _mm_add_epi32(lo, hi);
    hsum = _mm_hadd_epi32(hsum, hsum);
    hsum = _mm_hadd_epi32(hsum, hsum);
    return _mm_cvtsi128_si32(hsum);
#else
    int32_t sum = 0;
    for (int i = 0; i < len; ++i) {
        sum += static_cast<int32_t>(a[i]) * static_cast<int32_t>(w[i]);
    }
    return sum;
#endif
}

static inline int64_t dot_i16_i16(const int16_t* a, const int16_t* w, int len) {
    int64_t total = 0;
#if defined(__AVX2__)
    // Chunk every 64 elements (4 AVX2 vectors) so int32 lane accumulator stays <= ~534M
    for (int base = 0; base < len; base += 64) {
        __m256i sum = _mm256_setzero_si256();
        int end = std::min(base + 64, len);
        for (int i = base; i < end; i += 16) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a[i]));
            __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&w[i]));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(va, vw));
        }
        __m128i lo = _mm256_castsi256_si128(sum);
        __m128i hi = _mm256_extracti128_si256(sum, 1);
        __m128i hsum = _mm_add_epi32(lo, hi);
        hsum = _mm_hadd_epi32(hsum, hsum);
        hsum = _mm_hadd_epi32(hsum, hsum);
        total += static_cast<int64_t>(_mm_cvtsi128_si32(hsum));
    }
#else
    for (int i = 0; i < len; ++i) {
        total += static_cast<int64_t>(a[i]) * static_cast<int64_t>(w[i]);
    }
#endif
    return total;
}

static int evaluate_v5(const Board& board, const Accumulator& acc) {
    const int16_t* stmAcc  = acc.v[board.side_to_move()];
    const int16_t* nstmAcc = acc.v[~board.side_to_move()];

    // Pack 1024 CP-HG (Stream A) + 128 RelGeo (Stream B) scaled to [0, QA]
    alignas(32) int16_t feat1152[MLP_INPUT_SIZE];

#if defined(__AVX2__)
    const __m256i vZero = _mm256_setzero_si256();
    const __m256i vQa   = _mm256_set1_epi16(QA);
    const __m256i vOne  = _mm256_set1_epi16(1);

    // Exact (v / 255) for 0 <= v <= 65025 (unsigned uint16 arithmetic)
    auto screlu_div_qa = [&](__m256i prod) -> __m256i {
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(prod, vOne), _mm256_srli_epi16(prod, 8)), 8);
    };

    // Zone 1 (0..255)
    for (int i = 0; i < 256; i += 16) {
        __m256i s = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&stmAcc[i])), vZero), vQa);
        __m256i n = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i])), vZero), vQa);
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[i]),
                           screlu_div_qa(_mm256_mullo_epi16(s, s)));
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[512 + i]),
                           screlu_div_qa(_mm256_mullo_epi16(n, n)));
    }
    // Zone 2 (256..383)
    for (int i = 256; i < 384; i += 16) {
        __m256i s = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&stmAcc[i])), vZero), vQa);
        __m256i n = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i])), vZero), vQa);
        __m256i diff = _mm256_sub_epi16(s, n);
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[i]),
                           screlu_div_qa(_mm256_mullo_epi16(s, n)));
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[512 + i]),
                           screlu_div_qa(_mm256_mullo_epi16(diff, diff)));
    }
    // Zone 3 (384..511)
    for (int i = 384; i < 512; i += 16) {
        __m256i s = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&stmAcc[i])), vZero), vQa);
        __m256i n = _mm256_min_epi16(_mm256_max_epi16(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(&nstmAcc[i])), vZero), vQa);
        __m256i s_nb = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(s, 0xB1), 0xB1);
        __m256i n_nb = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(n, 0xB1), 0xB1);
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[i]),
                           screlu_div_qa(_mm256_mullo_epi16(s, n_nb)));
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[512 + i]),
                           screlu_div_qa(_mm256_mullo_epi16(n, s_nb)));
    }

    // Stream B: Register-resident RelGeo accumulation at leaf
    int relIdx[64];
    int relCount = 0;
    extract_relgeo_features(board, relIdx, relCount);

    __m256i r0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[0]));
    __m256i r1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[16]));
    __m256i r2 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[32]));
    __m256i r3 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[48]));
    __m256i r4 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[64]));
    __m256i r5 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[80]));
    __m256i r6 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[96]));
    __m256i r7 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&relBiases[112]));
    for (int r = 0; r < relCount; ++r) {
        const __m256i* rw = reinterpret_cast<const __m256i*>(relWeights[relIdx[r]]);
        r0 = _mm256_add_epi16(r0, _mm256_load_si256(rw + 0));
        r1 = _mm256_add_epi16(r1, _mm256_load_si256(rw + 1));
        r2 = _mm256_add_epi16(r2, _mm256_load_si256(rw + 2));
        r3 = _mm256_add_epi16(r3, _mm256_load_si256(rw + 3));
        r4 = _mm256_add_epi16(r4, _mm256_load_si256(rw + 4));
        r5 = _mm256_add_epi16(r5, _mm256_load_si256(rw + 5));
        r6 = _mm256_add_epi16(r6, _mm256_load_si256(rw + 6));
        r7 = _mm256_add_epi16(r7, _mm256_load_si256(rw + 7));
    }
    auto store_rel = [&](int offset, __m256i v) {
        __m256i c = _mm256_min_epi16(_mm256_max_epi16(v, vZero), vQa);
        _mm256_store_si256(reinterpret_cast<__m256i*>(&feat1152[OUTPUT_INPUT_SIZE + offset]),
                           screlu_div_qa(_mm256_mullo_epi16(c, c)));
    };
    store_rel(0, r0);
    store_rel(16, r1);
    store_rel(32, r2);
    store_rel(48, r3);
    store_rel(64, r4);
    store_rel(80, r5);
    store_rel(96, r6);
    store_rel(112, r7);

    auto hsum_epi32 = [](__m256i s) -> int32_t {
        __m128i lo = _mm256_castsi256_si128(s);
        __m128i hi = _mm256_extracti128_si256(s, 1);
        __m128i h = _mm_add_epi32(lo, hi);
        h = _mm_hadd_epi32(h, h);
        h = _mm_hadd_epi32(h, h);
        return _mm_cvtsi128_si32(h);
    };

    // Fusion Bottleneck: 4-row tiled AVX2 (1152 -> 32 + SCReLU)
    alignas(32) int16_t h32[MLP_HIDDEN];
    for (int k = 0; k < MLP_HIDDEN; k += 4) {
        __m256i s0 = _mm256_setzero_si256();
        __m256i s1 = _mm256_setzero_si256();
        __m256i s2 = _mm256_setzero_si256();
        __m256i s3 = _mm256_setzero_si256();
        const int8_t* w0 = mlp1Weights[k];
        const int8_t* w1 = mlp1Weights[k + 1];
        const int8_t* w2 = mlp1Weights[k + 2];
        const int8_t* w3 = mlp1Weights[k + 3];
        for (int i = 0; i < MLP_INPUT_SIZE; i += 16) {
            __m256i va = _mm256_load_si256(reinterpret_cast<const __m256i*>(&feat1152[i]));
            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                va, _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(&w0[i])))));
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                va, _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(&w1[i])))));
            s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(
                va, _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(&w2[i])))));
            s3 = _mm256_add_epi32(s3, _mm256_madd_epi16(
                va, _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(&w3[i])))));
        }
        int32_t z[4] = {
            hsum_epi32(s0) + mlp1Biases[k],
            hsum_epi32(s1) + mlp1Biases[k + 1],
            hsum_epi32(s2) + mlp1Biases[k + 2],
            hsum_epi32(s3) + mlp1Biases[k + 3],
        };
        for (int r = 0; r < 4; ++r) {
            int32_t hc = std::clamp(z[r] / QB, 0, QA);
            h32[k + r] = static_cast<int16_t>((hc * hc) / QA);
        }
    }
#else
    // Zone 1 (0..255)
    for (int i = 0; i < 256; ++i) {
        int32_t s = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        feat1152[i]       = static_cast<int16_t>((s * s) / QA);
        feat1152[512 + i] = static_cast<int16_t>((n * n) / QA);
    }
    // Zone 2 (256..383)
    for (int i = 256; i < 384; ++i) {
        int32_t s = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        int32_t diff = s - n;
        feat1152[i]       = static_cast<int16_t>((s * n) / QA);
        feat1152[512 + i] = static_cast<int16_t>((diff * diff) / QA);
    }
    // Zone 3 (384..511)
    for (int i = 384; i < 512; ++i) {
        int32_t s    = std::clamp(static_cast<int32_t>(stmAcc[i]), 0, QA);
        int32_t n    = std::clamp(static_cast<int32_t>(nstmAcc[i]), 0, QA);
        int32_t s_nb = std::clamp(static_cast<int32_t>(stmAcc[i ^ 1]), 0, QA);
        int32_t n_nb = std::clamp(static_cast<int32_t>(nstmAcc[i ^ 1]), 0, QA);
        feat1152[i]       = static_cast<int16_t>((s * n_nb) / QA);
        feat1152[512 + i] = static_cast<int16_t>((n * s_nb) / QA);
    }

    // Stream B: Sparse RelGeo accumulation at leaf
    int relIdx[64];
    int relCount = 0;
    extract_relgeo_features(board, relIdx, relCount);

    alignas(32) int16_t relAcc[RELGEO_HIDDEN];
    std::memcpy(relAcc, relBiases, sizeof(relAcc));
    for (int r = 0; r < relCount; ++r) {
        const int16_t* rw = relWeights[relIdx[r]];
        for (int j = 0; j < RELGEO_HIDDEN; ++j) {
            relAcc[j] += rw[j];
        }
    }
    for (int j = 0; j < RELGEO_HIDDEN; ++j) {
        int32_t rc = std::clamp(static_cast<int32_t>(relAcc[j]), 0, QA);
        feat1152[OUTPUT_INPUT_SIZE + j] = static_cast<int16_t>((rc * rc) / QA);
    }

    // Fusion Bottleneck: 1152 -> 32 + SCReLU
    alignas(32) int16_t h32[MLP_HIDDEN];
    for (int k = 0; k < MLP_HIDDEN; ++k) {
        int32_t z = dot_i16_i8(feat1152, mlp1Weights[k], MLP_INPUT_SIZE) + mlp1Biases[k];
        int32_t hc = std::clamp(z / QB, 0, QA);
        h32[k] = static_cast<int16_t>((hc * hc) / QA);
    }
#endif

    int pieceCount = popcount(board.pieces());
    int p = std::clamp(pieceCount, 2, 32) - 1;
    int b0 = p / 4;
    int rem = p % 4;
    int b1 = std::min(b0 + 1, OUTPUT_BUCKETS - 1);

    int64_t vBlended = 0;
    int64_t dBlended = 0;
    double denom = static_cast<double>(QA) * static_cast<double>(QB);

    if (b0 != b1 && rem != 0) {
        int64_t skip0 = 0, skip1 = 0;
        const int16_t* ow0 = outputWeights[b0];
        const int16_t* ow1 = outputWeights[b1];
#if defined(__AVX2__)
        for (int base = 0; base < OUTPUT_INPUT_SIZE; base += 64) {
            __m256i s0 = _mm256_setzero_si256();
            __m256i s1 = _mm256_setzero_si256();
            for (int i = base; i < base + 64; i += 16) {
                __m256i va = _mm256_load_si256(reinterpret_cast<const __m256i*>(&feat1152[i]));
                s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                    va, _mm256_load_si256(reinterpret_cast<const __m256i*>(&ow0[i]))));
                s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                    va, _mm256_load_si256(reinterpret_cast<const __m256i*>(&ow1[i]))));
            }
            skip0 += static_cast<int64_t>(hsum_epi32(s0));
            skip1 += static_cast<int64_t>(hsum_epi32(s1));
        }
#else
        skip0 = dot_i16_i16(feat1152, ow0, OUTPUT_INPUT_SIZE);
        skip1 = dot_i16_i16(feat1152, ow1, OUTPUT_INPUT_SIZE);
#endif
        int64_t v0 = skip0 + dot_i16_i16(h32, mlpOutVWeights[b0], MLP_HIDDEN) + static_cast<int64_t>(outputBiases[b0]);
        int64_t d0 = dot_i16_i16(h32, mlpOutDWeights[b0], MLP_HIDDEN) + static_cast<int64_t>(outDBiases[b0]);
        int64_t v1 = skip1 + dot_i16_i16(h32, mlpOutVWeights[b1], MLP_HIDDEN) + static_cast<int64_t>(outputBiases[b1]);
        int64_t d1 = dot_i16_i16(h32, mlpOutDWeights[b1], MLP_HIDDEN) + static_cast<int64_t>(outDBiases[b1]);
        vBlended = v0 * (4 - rem) + v1 * rem;
        dBlended = d0 * (4 - rem) + d1 * rem;
        denom *= 4.0;
    } else {
        int64_t skip0 = dot_i16_i16(feat1152, outputWeights[b0], OUTPUT_INPUT_SIZE);
        vBlended = skip0 + dot_i16_i16(h32, mlpOutVWeights[b0], MLP_HIDDEN) + static_cast<int64_t>(outputBiases[b0]);
        dBlended = dot_i16_i16(h32, mlpOutDWeights[b0], MLP_HIDDEN) + static_cast<int64_t>(outDBiases[b0]);
    }

    double vCp = (static_cast<double>(vBlended) * SCALE) / denom;
    double dLogit = static_cast<double>(dBlended) / denom;
    double drawProb = 1.0 / (1.0 + std::exp(-dLogit));
    return static_cast<int>(std::llround(vCp * (1.0 - drawProb)));
}

int evaluate(const Accumulator& acc, Color sideToMove, int bucket) {
    if (!networkLoaded) return 0;
    if (bucket < 0) bucket = 0;
    if (bucket >= OUTPUT_BUCKETS) bucket = OUTPUT_BUCKETS - 1;

    const int16_t* stmAcc  = acc.v[sideToMove];
    const int16_t* nstmAcc = acc.v[~sideToMove];

    if (networkVersion >= 4) {
        int32_t raw = forward_raw_v4_bucket(stmAcc, nstmAcc, bucket);
        int32_t out = raw / QA + outputBiases[bucket];
        return static_cast<int>((static_cast<int64_t>(out) * SCALE) / (QA * QB));
    }

    const int16_t* stmWeights  = &outputWeights[bucket][0];
    const int16_t* nstmWeights = &outputWeights[bucket][HIDDEN_SIZE];
    int32_t bias = outputBiases[bucket];

#if defined(__AVX2__)
    return forward_avx2(stmAcc, nstmAcc, stmWeights, nstmWeights, bias);
#else
    return forward_scalar(stmAcc, nstmAcc, stmWeights, nstmWeights, bias);
#endif
}

int evaluate(const Board& board, const Accumulator& acc) {
    if (!networkLoaded) return 0;
    if (networkVersion >= 5) {
        return evaluate_v5(board, acc);
    }
    int pieceCount = popcount(board.pieces());

    if (networkVersion >= 4) {
        const int16_t* stmAcc  = acc.v[board.side_to_move()];
        const int16_t* nstmAcc = acc.v[~board.side_to_move()];

        int p = std::clamp(pieceCount, 2, 32) - 1;
        int b0 = p / 4;
        int rem = p % 4;
        int b1 = std::min(b0 + 1, OUTPUT_BUCKETS - 1);

        int32_t raw0 = forward_raw_v4_bucket(stmAcc, nstmAcc, b0);
        int32_t out0 = raw0 / QA + outputBiases[b0];

        if (b0 == b1 || rem == 0) {
            return static_cast<int>((static_cast<int64_t>(out0) * SCALE) / (QA * QB));
        }

        int32_t raw1 = forward_raw_v4_bucket(stmAcc, nstmAcc, b1);
        int32_t out1 = raw1 / QA + outputBiases[b1];
        int64_t blended = static_cast<int64_t>(out0) * (4 - rem) + static_cast<int64_t>(out1) * rem;
        return static_cast<int>((blended * SCALE) / (4 * QA * QB));
    }

    int bucket = output_bucket(pieceCount);
    return evaluate(acc, board.side_to_move(), bucket);
}

int evaluate(const Board& board) {
    if (!networkLoaded) return 0;
    Accumulator acc;
    refresh_accumulator(board, acc);
    return evaluate(board, acc);
}

bool verify_accumulator(const Board& board, const Accumulator& acc) {
    Accumulator ref;
    refresh_accumulator(board, ref);
    return std::memcmp(&acc, &ref, sizeof(Accumulator)) == 0;
}

} // namespace NNUE
