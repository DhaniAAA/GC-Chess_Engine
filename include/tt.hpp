#ifndef TT_HPP
#define TT_HPP

#include "types.hpp"
#include "zobrist.hpp"
#include "move.hpp"
#include <cstring>
#include <memory>
#include <xmmintrin.h>

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
    BOUND_UPPER = 1,   // Score is upper bound (failed low, all-node)
    BOUND_LOWER = 2,   // Score is lower bound (failed high, cut-node)
    BOUND_EXACT = 3    // Score is exact (PV-node)
};

struct TTEntry {
    U16 key16;        // Upper 16 bits of position key
    U16 move16;       // Best move
    S16 score16;      // Search score
    S16 eval16;       // Static evaluation
    U8  depth8;       // Search depth
    U8  genBound8;    // Generation (6 bits) + Bound (2 bits)

    Move move() const { return Move(move16); }
    int score() const { return score16; }
    int eval() const { return eval16; }
    int depth() const { return depth8; }
    Bound bound() const { return Bound(genBound8 & 0x3); }
    U8 generation() const { return genBound8 >> 2; }

    void save(Key k, int s, int e, Bound b, int d, Move m, U8 gen) {
        // Preserve move if we don't have a new one
        if (m || (k >> 48) != key16) {
            move16 = m.raw();
        }

        // Don't overwrite entries with higher depth from same search
        if (b == BOUND_EXACT || (k >> 48) != key16 || d + 4 > depth8) {
            key16 = static_cast<U16>(k >> 48);
            score16 = static_cast<S16>(s);
            eval16 = static_cast<S16>(e);
            depth8 = static_cast<U8>(d);
            genBound8 = static_cast<U8>((gen << 2) | b);
        }
    }
};

static_assert(sizeof(TTEntry) == 10, "TTEntry size should be 10 bytes");

// ============================================================================
// Transposition Table Cluster
//
// Each cluster contains multiple entries to handle hash collisions.
// Using 3 entries per cluster for a good balance.
// ============================================================================

struct TTCluster {
    static constexpr int ENTRIES_PER_CLUSTER = 3;
    TTEntry entries[ENTRIES_PER_CLUSTER];
    char padding[2];  // Pad to 32 bytes for cache line efficiency
};

static_assert(sizeof(TTCluster) == 32, "TTCluster should be 32 bytes");

// ============================================================================
// Transposition Table
// ============================================================================

class TranspositionTable {
public:
    TranspositionTable();
    ~TranspositionTable();

    // Resize the table (size in MB)
    void resize(size_t mb);

    // Clear the entire table
    void clear();

    // Prefetch TT entry into cache
    void prefetch(Key key) {
        #if defined(_MM_HINT_T0)
        _mm_prefetch((const char*)first_entry(key), _MM_HINT_T0);
        #elif defined(__GNUC__)
        __builtin_prefetch(first_entry(key));
        #endif
    }

    // Increment the generation (call at start of each search)
    void new_search() { generation8 += 4; }  // Shift by 4 to keep bound bits clear

    // Probe the table for a position
    // Returns pointer to entry if found, nullptr otherwise
    // Sets 'found' to true if the entry matches the key
    TTEntry* probe(Key key, bool& found);

    // Get estimated usage (permille, 0-1000)
    int hashfull() const;

    // Get current generation
    U8 generation() const { return generation8; }

private:
    TTCluster* table;
    size_t clusterCount;
    U8 generation8;

    TTEntry* first_entry(Key key) {
        return &table[key % clusterCount].entries[0];
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

// Adjust score for storing in TT (convert to root-relative)
inline int score_to_tt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) {
        return score + ply;
    }
    if (score <= VALUE_MATED_IN_MAX_PLY) {
        return score - ply;
    }
    return score;
}

// Adjust score retrieved from TT (convert to current-ply-relative)
inline int score_from_tt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) {
        return score - ply;
    }
    if (score <= VALUE_MATED_IN_MAX_PLY) {
        return score + ply;
    }
    return score;
}

// Global transposition table
extern TranspositionTable TT;

#endif // TT_HPP
