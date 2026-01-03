#include "tt.hpp"
#include "profiler.hpp"
#include <iostream>
#include <cstdlib>

TranspositionTable TT;

TranspositionTable::TranspositionTable()
    : table(nullptr), clusterCount(0), clusterMask(0), generation8(0) {
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

    for (int i = 0; i < TTCluster::ENTRIES_PER_CLUSTER; ++i) {
        if (entry[i].key16 == key16) {
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
        int age_diff = (generation8 - entry[i].generation()) & 0xFC;
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

    for (int i = 0; i < TTCluster::ENTRIES_PER_CLUSTER; ++i) {
        if (entry[i].key16 == key16) {
            Move m = entry[i].move();
            if (m != MOVE_NONE) {
                bool duplicate = false;
                for (int j = 0; j < count; ++j) {
                    if (moves[j] == m) {
                        duplicate = true;
                        break;
                    }
                }

                if (!duplicate && count < TTCluster::ENTRIES_PER_CLUSTER) {
                    moves[count++] = m;
                }
            }
        }
    }
}

int TranspositionTable::hashfull() const {
    int count = 0;
    const int samples = 1000;

    if (!table || clusterCount == 0) return 0;

    for (int i = 0; i < samples && i < static_cast<int>(clusterCount); ++i) {
        const TTEntry* entry = &table[i].entries[0];
        for (int j = 0; j < TTCluster::ENTRIES_PER_CLUSTER; ++j) {
            if (entry[j].key16 != 0) {
                ++count;
            }
        }
    }

    return count * 1000 / (samples * TTCluster::ENTRIES_PER_CLUSTER);
}
