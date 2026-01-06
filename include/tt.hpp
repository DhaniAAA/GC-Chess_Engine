#ifndef TT_HPP
#define TT_HPP

#include "types.hpp"
#include "zobrist.hpp"
#include "move.hpp"
#include <cstring>
#include <memory>
#include <xmmintrin.h>
// ============================================================================
// Cache Line Size Constant
// Most modern CPUs (Intel, AMD, ARM) use 64-byte cache lines
// ============================================================================

constexpr size_t CACHE_LINE_SIZE = 64;

// ============================================================================
// Transposition Table Entry
//
// Compact representation to minimize memory usage:
// - Key: 16-bit (upper bits of full Zobrist key, rest implied by index)
// - Move: 16-bit
// - Score: 16-bit
// - Depth: 8-bit
// - Bound & Age: 8-bit (2 bits bound, 6 bits age)
// Total: 8 bytes per entry
// ============================================================================

enum Bound : U8 {
    BOUND_NONE = 0,
    BOUND_UPPER = 1,
    BOUND_LOWER = 2,
    BOUND_EXACT = 3
};

struct alignas(16) TTEntry {
    U16 key16;
    U16 move16;
    S16 score16;
    S16 eval16;
    U8  depth8;
    U8  genBound8;
    U8  padding[6];

    Move move() const { return Move(move16); }
    int score() const { return score16; }
    int eval() const { return eval16; }
    int depth() const { return depth8; }
    Bound bound() const { return Bound(genBound8 & 0x3); }
    U8 generation() const { return genBound8 >> 2; }

    void save(Key k, int s, int e, Bound b, int d, Move m, U8 gen) {
        if (m || (k >> 48) != key16) {
            move16 = m.raw();
        }
        if (b == BOUND_EXACT || (k >> 48) != key16 || d + 4 > depth8) {
            key16 = static_cast<U16>(k >> 48);
            score16 = static_cast<S16>(s);
            eval16 = static_cast<S16>(e);
            depth8 = static_cast<U8>(d);
            genBound8 = static_cast<U8>((gen << 2) | b);
        }
    }
};

static_assert(sizeof(TTEntry) == 16, "TTEntry size should be 16 bytes");

// ============================================================================
// Transposition Table Cluster
// Each cluster contains multiple entries to handle hash collisions.
// Using 3 entries per cluster for a good balance.
// ============================================================================

struct alignas(CACHE_LINE_SIZE) TTCluster {
    static constexpr int ENTRIES_PER_CLUSTER = 4;
    TTEntry entries[ENTRIES_PER_CLUSTER];
};

static_assert(sizeof(TTCluster) == CACHE_LINE_SIZE, "TTCluster should be 64 bytes (1 cache line)");
static_assert(alignof(TTCluster) == CACHE_LINE_SIZE, "TTCluster should be cache-line aligned");

// ============================================================================
// Transposition Table
// ============================================================================

class TranspositionTable {
public:
    TranspositionTable();
    ~TranspositionTable();

    void resize(size_t mb);
    void clear();

    void prefetch(Key key) {
        #if defined(_MM_HINT_T0)
        _mm_prefetch((const char*)first_entry(key), _MM_HINT_T0);
        #elif defined(__GNUC__)
        __builtin_prefetch(first_entry(key));
        #endif
    }

    void prefetch2(Key key) {
        #if defined(_MM_HINT_T1)
        _mm_prefetch((const char*)first_entry(key), _MM_HINT_T1);
        #elif defined(__GNUC__)
        __builtin_prefetch(first_entry(key), 0, 2);
        #endif
    }

    void new_search() { generation8 += 4; }

    TTEntry* probe(Key key, bool& found);
    void get_moves(Key key, Move* moves, int& count);
    int hashfull() const;
    U8 generation() const { return generation8; }

private:
    TTCluster* table;
    size_t clusterCount;
    size_t clusterMask;
    U8 generation8;

    TTEntry* first_entry(Key key) {
        return &table[key & clusterMask].entries[0];
    }
};

// ============================================================================
// Score Adjustment for TT Storage
//
// Mate scores need to be adjusted based on ply to ensure correct
// mate distance reporting regardless of where the position was found.
// ============================================================================

constexpr int VALUE_MATE = 32000;
constexpr int VALUE_MATE_IN_MAX_PLY = VALUE_MATE - 100;
constexpr int VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;
constexpr int VALUE_TB_WIN = VALUE_MATE_IN_MAX_PLY - 1;
constexpr int VALUE_TB_LOSS = -VALUE_TB_WIN;
constexpr int VALUE_NONE = 32001;
constexpr int VALUE_INFINITE = 32002;

inline int score_to_tt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) {
        return score + ply;
    }
    if (score <= VALUE_MATED_IN_MAX_PLY) {
        return score - ply;
    }
    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) {
        return score - ply;
    }
    if (score <= VALUE_MATED_IN_MAX_PLY) {
        return score + ply;
    }
    return score;
}

extern TranspositionTable TT;

#endif
