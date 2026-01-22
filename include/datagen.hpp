// ============================================================================
// datagen.hpp - Data Generation for NNUE Training
// ============================================================================
// This module generates training data for NNUE by playing self-games using
// the current engine's evaluation and search.
// ============================================================================

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
// Training Data Entry
// ============================================================================
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

// ============================================================================
// Data Generator Configuration
// ============================================================================

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
    bool skip_captures = true;
    bool skip_tactical_bestmove = true;
    int max_score = 2500;
    int eval_limit = 0;

    int qsearch_margin = 60;
    int search_margin = 70;

    std::string output = "data/training.binpack";
    int flush_interval = 10000;

    bool use_book = true;
    std::string book_path = "book/Perfect2023.bin";
    int book_depth = 12;
    int random_multi_pv = 2;
};

// ============================================================================
// Statistics
// ============================================================================

struct DataGenStats {
    std::atomic<uint64_t> games_started{0};
    std::atomic<uint64_t> games_completed{0};
    std::atomic<uint64_t> games_white_wins{0};
    std::atomic<uint64_t> games_black_wins{0};
    std::atomic<uint64_t> games_draws{0};
    std::atomic<uint64_t> positions_generated{0};
    std::atomic<uint64_t> positions_filtered{0};
    std::atomic<uint64_t> total_plies{0};

    void reset() {
        games_started = 0;
        games_completed = 0;
        games_white_wins = 0;
        games_black_wins = 0;
        games_draws = 0;
        positions_generated = 0;
        positions_filtered = 0;
        total_plies = 0;
    }

    void print() const;
};

// ============================================================================
// Game Result
// ============================================================================

enum class GameResult {
    ongoing = 0,
    white_wins = 1,
    black_wins = 2,
    draw = 3
};

// ============================================================================
// Data Generator Class
// ============================================================================

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

    void worker_thread(int thread_id);
    GameResult play_game(std::vector<TrainingEntry>& entries, int thread_id);

    TrainingEntry encode_position(const Board& board, int score, GameResult result);

    Move select_random_move(Board& board, int thread_id);
    Move select_search_move(Board& board, int& score, int thread_id);

    bool should_record_position(Board& board, int static_eval, int search_score, int ply, Move best_move, int thread_id);

    void write_entries(const std::vector<TrainingEntry>& entries);
    void flush_output();

    std::vector<uint64_t> m_random_seeds;
    uint64_t rand_next(int thread_id);
    int rand_int(int thread_id, int max);
    std::vector<std::unique_ptr<Search>> m_searchers;
};

// ============================================================================
// Marlinformat Conversion (for Bullet trainer compatibility)
// ============================================================================

void to_marlinformat(const TrainingEntry& entry, std::vector<uint8_t>& output);

// ============================================================================
// Global Interface (used by UCI)
// ============================================================================

void start(const DataGenConfig& config);
void stop();
bool is_running();
const DataGenStats& get_stats();

DataGenConfig parse_config(std::istringstream& is);

// ============================================================================
// Binpack File Reading & Conversion
// ============================================================================

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

// ============================================================================
// Filter Existing Data (Quiet Position Detection)
// ============================================================================

struct FilterConfig {
    std::string input_path;
    std::string output_path;
    int threads = 1;

    bool skip_in_check = true;
    bool skip_tactical_bestmove = true;
    int qsearch_margin = 60;
    int search_margin = 0;
    int max_score = 2500;
    int eval_limit = 0;

    int report_interval = 100000;
};

struct FilterStats {
    size_t total_read = 0;
    size_t passed = 0;
    size_t filtered_check = 0;
    size_t filtered_tactical = 0;
    size_t filtered_qsearch = 0;
    size_t filtered_score = 0;
    size_t clamped_eval_limit = 0;
};

bool filter_binpack(const FilterConfig& config, FilterStats& stats);
FilterConfig parse_filter_config(std::istringstream& is);

} // namespace DataGen

#endif // DATAGEN_HPP
