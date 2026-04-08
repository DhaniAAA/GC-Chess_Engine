#include "tt.hpp"
#include "profiler.hpp"
#include <iostream>
#include <cstdlib>

TranspositionTable TT;

TranspositionTable::TranspositionTable()
    : table(nullptr), clusterCount(0), clusterMask(0), generation8(0) {
    // Default size — keep in sync with uci.hpp::EngineOptions::hash
    resize(128);
}

TranspositionTable::~TranspositionTable() {
    if (table) {
#ifdef _WIN32
        _aligned_free(table);
#else
        std::free(table);
#endif
    }
}

void TranspositionTable::resize(size_t mb) {
    if (table) {
#ifdef _WIN32
        _aligned_free(table);
#else
        std::free(table);
#endif
        table = nullptr;
    }

    size_t sizeBytes = mb * 1024 * 1024;
    size_t targetCount = sizeBytes / sizeof(TTCluster);

    clusterCount = 1;
    while (clusterCount * 2 <= targetCount) {
        clusterCount *= 2;
    }

    clusterMask = clusterCount - 1;

#ifdef _WIN32
    table = static_cast<TTCluster*>(_aligned_malloc(clusterCount * sizeof(TTCluster), 64));
#else
    table = static_cast<TTCluster*>(std::aligned_alloc(64, clusterCount * sizeof(TTCluster)));
#endif

    if (!table) {
        std::cerr << "Failed to allocate transposition table\n";
        clusterCount = 0;
        return;
    }

    clear();
}

void TranspositionTable::clear() {
    if (table && clusterCount > 0) {
        std::memset(table, 0, clusterCount * sizeof(TTCluster));
    }
    generation8 = 0;
}

TTEntry* TranspositionTable::probe(Key key, bool& found) {
    PROFILE_SCOPE("TT::probe");
    if (!table || clusterCount == 0) {
        found = false;
        return nullptr;
    }

    TTEntry* entry = first_entry(key);
    U16 key16 = static_cast<U16>(key >> 48);
    U32 key32 = static_cast<U32>(key >> 16);  // CRITICAL FIX: Extract key32 for verification

    for (int i = 0; i < TTCluster::ENTRIES_PER_CLUSTER; ++i) {
        // CRITICAL FIX: Verify BOTH key16 AND key32 to prevent collisions
        // Old: Only checked key16 (1/65K collision rate)
        // New: Check key16 + key32 (1/281 trillion collision rate)
        if (entry[i].key16 == key16 && entry[i].key32 == key32) {
            found = true;
            return &entry[i];
        }

        if (entry[i].key16 == 0) {
            found = false;
            return &entry[i];
        }
    }

    TTEntry* replace = &entry[0];
    int best_score = -100000;

    for (int i = 0; i < TTCluster::ENTRIES_PER_CLUSTER; ++i) {
        // age_diff: compare in shifted format (both with bound bits masked out)
        // generation8 has format (gen << 2), entry genBound8 has (gen << 2 | bound)
        // Mask genBound8 to get just gen bits, then subtract
        int age_diff = (generation8 - (entry[i].genBound8 & 0xFC)) & 0xFC;
        int replace_score = age_diff * 256 - entry[i].depth8;

        if (replace_score > best_score) {
            best_score = replace_score;
            replace = &entry[i];
        }
    }

    found = false;
    return replace;
}

void TranspositionTable::get_moves(Key key, Move* moves, int& count) {
    count = 0;
    if (!table || clusterCount == 0) return;

    TTEntry* entry = first_entry(key);
    U16 key16 = static_cast<U16>(key >> 48);
    U32 key32 = static_cast<U32>(key >> 16);

    Move bestMove = MOVE_NONE;
    int bestDepth = -1;

    for (int i = 0; i < TTCluster::ENTRIES_PER_CLUSTER; ++i) {
        if (entry[i].key16 == key16 && entry[i].key32 == key32) {
            Move m = entry[i].move();
            if (m != MOVE_NONE) {
                int d = entry[i].depth();
                if (d > bestDepth) {
                    bestDepth = d;
                    bestMove = m;
                }
            }
        }
    }

    if (bestMove != MOVE_NONE) {
        moves[count++] = bestMove;
    }
}

int TranspositionTable::hashfull() const {
    int count = 0;
    const int samples = 1000;

    if (!table || clusterCount == 0) return 0;

    // Sample the first 1000 clusters and count entries that belong
    // to the CURRENT generation. generation8 is pre-shifted (+=4 per search),
    // with the low 2 bits reserved for Bound. entry.generation() returns
    // genBound8 >> 2, which is the raw generation counter.
    U8 currentGen = generation8 >> 2;
    for (int i = 0; i < samples && i < static_cast<int>(clusterCount); ++i) {
        const TTEntry* entry = &table[i].entries[0];
        for (int j = 0; j < TTCluster::ENTRIES_PER_CLUSTER; ++j) {
            if (entry[j].depth8 != 0 && entry[j].generation() == currentGen) {
                ++count;
            }
        }
    }

    return count * 1000 / (samples * TTCluster::ENTRIES_PER_CLUSTER);
}


