#include "tt.hpp"
#include <iostream>
#include <cstdlib>

// ============================================================================
// Global Transposition Table Instance
// ============================================================================

TranspositionTable TT;

// ============================================================================
// Constructor / Destructor
// ============================================================================

TranspositionTable::TranspositionTable()
    : table(nullptr), clusterCount(0), generation8(0) {
    resize(16);  // Default 16 MB
}

TranspositionTable::~TranspositionTable() {
    if (table) {
#ifdef _WIN32
        _aligned_free(table); // [PERBAIKAN] Gunakan _aligned_free di Windows
#else
        std::free(table);
#endif
    }
}
// ============================================================================
// Resize
// ============================================================================

void TranspositionTable::resize(size_t mb) {
    if (table) {
#ifdef _WIN32
        _aligned_free(table);
#else
        std::free(table);
#endif
        table = nullptr;
    }

    // Calculate number of clusters
    clusterCount = mb * 1024 * 1024 / sizeof(TTCluster);

    // Allocate aligned memory for cache efficiency
#ifdef _WIN32
    table = static_cast<TTCluster*>(_aligned_malloc(clusterCount * sizeof(TTCluster), 64));
#else
    table = static_cast<TTCluster*>(std::aligned_alloc(64, clusterCount * sizeof(TTCluster)));
#endif

    if (!table) {
        std::cerr << "Failed to allocate " << mb << "MB for transposition table\n";
        clusterCount = 0;
        return;
    }

    clear();

    // [PERBAIKAN] Output ini dikomentari karena mengganggu protokol UCI.
    // GUI mengharapkan engine diam sampai menerima perintah "uci".
    // std::cout << "Transposition table: " << mb << " MB, "
    //           << clusterCount << " clusters, "
    //           << (clusterCount * TTCluster::ENTRIES_PER_CLUSTER) << " entries\n";
}

// ============================================================================
// Clear
// ============================================================================

void TranspositionTable::clear() {
    if (table && clusterCount > 0) {
        std::memset(table, 0, clusterCount * sizeof(TTCluster));
    }
    generation8 = 0;
}

// ============================================================================
// Probe
// ============================================================================

TTEntry* TranspositionTable::probe(Key key, bool& found) {
    if (!table || clusterCount == 0) {
        found = false;
        return nullptr;
    }

    TTEntry* entry = first_entry(key);
    U16 key16 = static_cast<U16>(key >> 48);

    // Search through cluster entries
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

    // Find the best entry to replace using a replacement scheme:
    // Prefer to replace entries that are:
    // 1. From an older generation
    // 2. Have shallower depth
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

// ============================================================================
// Hash Usage
// ============================================================================

int TranspositionTable::hashfull() const {
    int count = 0;
    const int samples = 1000;

    if (!table || clusterCount == 0) return 0;

    for (int i = 0; i < samples; ++i) {
        const TTEntry* entry = &table[i % clusterCount].entries[0]; // Safety check added above, and modulus added just in case
        for (int j = 0; j < TTCluster::ENTRIES_PER_CLUSTER; ++j) {
            if (entry[j].depth8 != 0 && entry[j].generation() == generation8) {
                ++count;
            }
        }
    }

    return count * 1000 / (samples * TTCluster::ENTRIES_PER_CLUSTER);
}
