#ifndef DATAGEN_HPP
#define DATAGEN_HPP

#include "board.hpp"
#include "move.hpp"
#include "types.hpp"
#include "search.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <fstream>
#include <cstdint>

namespace DataGen {

// ============================================================================
// ZobristHashSet — Open-addressing flat hash table untuk deduplication.
// Jauh lebih hemat RAM vs std::unordered_set:
//   std::unordered_set: ~40 bytes/elemen (node-based, heap alloc per entry)
//   ZobristHashSet    :  ~8 bytes/elemen (flat array of uint64_t)
// Untuk 100M posisi: 800MB vs ~4GB — hemat 5x!
//
// Menggunakan linear probing dengan load factor 50%.
// Sentinel: 0 = empty slot, UINT64_MAX = reserved (jangan masukkan key ini).
// ============================================================================
class ZobristHashSet {
public:
    explicit ZobristHashSet(size_t capacity = 0) {
        if (capacity > 0) {
            // Round up ke power-of-2 untuk bitmask
            size_t sz = 1;
            while (sz < capacity * 2) sz <<= 1;  // Load factor 50%
            m_table.assign(sz, EMPTY);
            m_mask = sz - 1;
        }
    }

    // Coba insert key. Return true jika berhasil (belum ada), false jika duplikat.
    bool insert(uint64_t key) {
        if (m_table.empty()) return true;  // Disabled mode: always pass
        if (key == EMPTY) key = EMPTY_ALT; // Remap sentinel

        size_t idx = hash(key) & m_mask;
        while (true) {
            uint64_t slot = m_table[idx];
            if (slot == EMPTY) {
                m_table[idx] = key;
                return true;   // Posisi baru
            }
            if (slot == key) {
                return false;  // Duplikat
            }
            idx = (idx + 1) & m_mask;  // Linear probing
        }
    }

    void clear() {
        std::fill(m_table.begin(), m_table.end(), EMPTY);
    }

    size_t capacity() const { return m_table.size(); }
    size_t memory_bytes() const { return m_table.size() * sizeof(uint64_t); }

private:
    static constexpr uint64_t EMPTY     = 0ULL;
    static constexpr uint64_t EMPTY_ALT = 0xDEADBEEFDEADBEEFULL; // Remap key==0

    // Fibonacci hashing untuk distribusi yang baik
    static size_t hash(uint64_t key) {
        return static_cast<size_t>(key * 0x9E3779B97F4A7C15ULL >> 32);
    }

    std::vector<uint64_t> m_table;
    size_t m_mask = 0;
};

#pragma pack(push, 1)
struct TrainingEntry {
    uint8_t packed_board[32];
    uint8_t stm;
    uint8_t castling;
    uint8_t ep_square;
    uint8_t rule50;
    uint8_t result;
    uint8_t padding;
    int16_t score;
};
#pragma pack(pop)

static_assert(sizeof(TrainingEntry) == 40, "TrainingEntry should be 40 bytes");

constexpr uint8_t PTYPE_EMPTY = 0;
constexpr uint8_t PTYPE_WP = 1, PTYPE_WN = 2, PTYPE_WB = 3, PTYPE_WR = 4, PTYPE_WQ = 5, PTYPE_WK = 6;
constexpr uint8_t PTYPE_BP = 7, PTYPE_BN = 8, PTYPE_BB = 9, PTYPE_BR = 10, PTYPE_BQ = 11, PTYPE_BK = 12;

struct DataGenConfig {
    int threads = 2;
    int hash_mb = 64;

    int depth = 8;
    int nodes = 0;
    int soft_nodes = 5000;

    int games = 100000;
    int random_plies = 8;
    int max_ply = 400;
    int min_ply = 16;

    int adjudicate_score = 2500;
    int adjudicate_count = 4;
    int adjudicate_draw = 5;
    int adjudicate_draw_count = 12;
    int adjudicate_draw_ply = 80;

    bool skip_in_check = true;
    bool skip_captures = false;          // Dinonaktifkan — redundan dengan skip_tactical_bestmove
    bool skip_tactical_bestmove = true;  // Filter komprehensif: skip jika bestmove adalah capture/promosi
    int  min_pieces = 5;                 // Skip endgame trivial: ≤5 piece (dari ≤3 sebelumnya)
    int  max_score = 2000;
    int  eval_limit = 0;

    int qsearch_margin = 100;
    int search_margin = 70;

    float score_lambda = 1.0f;
    int   wdl_scale    = 400;   // Centipawn scale sigmoid: sigmoid(cp/scale)
                                // ~400 cp = ~73% win probability (referensi: Stockfish model)
    bool  use_rule50_decay = true; // Scale eval dengan (100-rule50)/100 sebelum mixing
                                    // Mengajarkan NNUE bahwa posisi mendekati 50-move draw = netral

    std::string output = "data/training.binpack";
    int flush_interval = 10000;

    bool use_book = true;
    std::string book_path = "book/Perfect2023.bin";
    int book_depth = 12;
    int random_multi_pv = 2;
};

struct DataGenStats {
    std::atomic<uint64_t> games_started{0};
    std::atomic<uint64_t> games_completed{0};
    std::atomic<uint64_t> games_white_wins{0};
    std::atomic<uint64_t> games_black_wins{0};
    std::atomic<uint64_t> games_draws{0};
    std::atomic<uint64_t> positions_generated{0};
    std::atomic<uint64_t> positions_filtered{0};
    std::atomic<uint64_t> positions_deduped{0};  // Posisi duplikat yang dibuang
    std::atomic<uint64_t> total_plies{0};

    void reset() {
        games_started = 0;
        games_completed = 0;
        games_white_wins = 0;
        games_black_wins = 0;
        games_draws = 0;
        positions_generated = 0;
        positions_filtered = 0;
        positions_deduped = 0;
        total_plies = 0;
    }

    void print() const;
};

enum class GameResult {
    ongoing = 0,
    white_wins = 1,
    black_wins = 2,
    draw = 3
};

class DataGenerator {
public:
    DataGenerator(const DataGenConfig& config);
    ~DataGenerator();

    void run();
    void stop();
    bool is_running() const { return running; }

    const DataGenStats& stats() const { return m_stats; }

private:
    DataGenConfig m_config;
    DataGenStats m_stats;

    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};

    std::ofstream m_output;
    std::mutex m_output_mutex;
    uint64_t m_write_count = 0;

    void worker_thread(int thread_id);
    GameResult play_game(std::vector<TrainingEntry>& entries, int thread_id);

    TrainingEntry encode_position(const Board& board, int score, GameResult result);

    Move select_random_move(Board& board, int thread_id);
    Move select_multipv_move(Board& board, int thread_id);
    Move select_search_move(Board& board, int& score, int thread_id);

    bool should_record_position(Board& board, int static_eval, int search_score, int ply, Move best_move, int thread_id,
                                 ZobristHashSet& seen_hashes);

    void write_entries(const std::vector<TrainingEntry>& entries);
    void flush_output();

    std::vector<uint64_t> m_random_seeds;
    uint64_t rand_next(int thread_id);
    int rand_int(int thread_id, int max);
    std::vector<std::unique_ptr<Search>> m_searchers;
};

void to_marlinformat(const TrainingEntry& entry, std::vector<uint8_t>& output);

void start(const DataGenConfig& config);
void stop();
bool is_running();
const DataGenStats& get_stats();

DataGenConfig parse_config(std::istringstream& is);

bool read_binpack_file(const std::string& path, std::vector<TrainingEntry>& entries, size_t max_entries = 0);
void view_binpack_file(const std::string& path, size_t count = 10, size_t offset = 0);
bool convert_to_epd(const std::string& binpack_path, const std::string& epd_path, size_t max_entries = 0);

struct FileStats {
    size_t total_entries = 0;
    size_t white_wins = 0;
    size_t black_wins = 0;
    size_t draws = 0;
    int64_t total_score = 0;
    int min_score = 0;
    int max_score = 0;
};
bool get_file_stats(const std::string& path, FileStats& stats);

std::string entry_to_string(const TrainingEntry& entry);
std::string entry_to_fen(const TrainingEntry& entry);
bool entry_to_board(const TrainingEntry& entry, Board& board, StateInfo& si);

struct FilterConfig {
    std::string input_path;
    std::string output_path;
    int threads = 1;

    bool skip_in_check = true;
    bool skip_tactical_bestmove = false;  // Expensive for post-hoc; enable dengan token "tactical_filter"
    int  tactical_search_depth = 1;       // Depth search untuk tactical filter post-hoc
    int  qsearch_margin = 60;
    int  search_margin = 0;
    int  max_score = 2000;                // Turun dari 2500 — sesuai standar modern
    int  min_pieces = 5;                  // Skip endgame trivial ≤5 piece
    int  eval_limit = 0;
    bool deduplicate = true;              // Buang posisi dengan Zobrist hash yang sama

    int report_interval = 100000;
};

struct FilterStats {
    size_t total_read = 0;
    size_t passed = 0;
    size_t filtered_check = 0;
    size_t filtered_tactical = 0;
    size_t filtered_qsearch = 0;
    size_t filtered_score = 0;
    size_t filtered_mate = 0;
    size_t filtered_few_pieces = 0;
    size_t filtered_duplicate = 0;  // Posisi duplikat (same Zobrist hash)
    size_t clamped_eval_limit = 0;
};

bool filter_binpack(const FilterConfig& config, FilterStats& stats);
FilterConfig parse_filter_config(std::istringstream& is);

}

#endif
