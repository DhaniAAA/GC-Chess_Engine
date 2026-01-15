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

// Binary format for training data (efficient storage)
// This format stores the complete chess position in a compact binary form.
//
// Encoding explanation:
// - packed_board[32]: stores all 64 squares, 4 bits per piece (2 per byte)
//   Piece type: 0=empty, 1=WP, 2=WN, 3=WB, 4=WR, 5=WQ, 6=WK, 7=BP, 8=BN, 9=BB, 10=BR, 11=BQ, 12=BK
//   Lower nibble = even square, Upper nibble = odd square
// - stm: side to move
// - castling: castling rights
// - ep_square: en passant square (64 = none)
// - rule50: halfmove clock
// - result: game outcome
// - score: evaluation in centipawns
//
#pragma pack(push, 1)
struct TrainingEntry {
    // Board state (packed representation - 64 squares, 4 bits each = 32 bytes)
    uint8_t packed_board[32];   // Packed piece types for all 64 squares

    // Metadata (8 bytes)
    uint8_t stm;                // Side to move: 0 = White, 1 = Black
    uint8_t castling;           // Castling rights (4 bits used)
    uint8_t ep_square;          // En passant square (64 = none)
    uint8_t rule50;             // 50-move rule counter (capped at 255)
    uint8_t result;             // Game result: 0 = Black wins, 1 = Draw, 2 = White wins
    uint8_t padding;            // Padding for alignment
    int16_t score;              // Evaluation score from white's perspective (centipawns)
};
#pragma pack(pop)

// Expected size: 32 + 8 = 40 bytes
static_assert(sizeof(TrainingEntry) == 40, "TrainingEntry should be 40 bytes");

// Piece type encoding for packed board (4 bits per piece)
// 0 = empty, 1-6 = white pieces (P,N,B,R,Q,K), 7-12 = black pieces (P,N,B,R,Q,K)
constexpr uint8_t PTYPE_EMPTY = 0;
constexpr uint8_t PTYPE_WP = 1, PTYPE_WN = 2, PTYPE_WB = 3, PTYPE_WR = 4, PTYPE_WQ = 5, PTYPE_WK = 6;
constexpr uint8_t PTYPE_BP = 7, PTYPE_BN = 8, PTYPE_BB = 9, PTYPE_BR = 10, PTYPE_BQ = 11, PTYPE_BK = 12;

// ============================================================================
// Data Generator Configuration
// ============================================================================

struct DataGenConfig {
    // Threading
    int threads = 1;                    // Number of worker threads
    int hash_mb = 16;                   // Hash table size in MB (per thread if separate TT)

    // Search settings
    int depth = 8;                      // Search depth for scoring positions
    int nodes = 0;                      // Node limit (0 = use depth limit instead)
    int soft_nodes = 5000;              // Soft node limit for faster generation

    // Game settings
    int games = 100000;                 // Total games to play
    int random_plies = 8;               // Random opening moves after book (half-moves)
    int max_ply = 400;                  // Maximum game length (half-moves)
    int min_ply = 16;                   // Minimum ply before recording positions

    // Adjudication
    int adjudicate_score = 2500;        // Score threshold for resignation (centipawns)
    int adjudicate_count = 4;           // Consecutive moves above threshold to adjudicate
    int adjudicate_draw = 5;            // Draw if |score| < 5 for this many moves
    int adjudicate_draw_count = 12;     // Number of low-score moves for draw
    int adjudicate_draw_ply = 80;       // Minimum ply before draw adjudication

    // Position filtering
    bool skip_in_check = true;          // Skip positions where side to move is in check
    bool skip_captures = false;         // Skip positions where best move is capture
    int max_score = 3000;               // Skip positions with |score| > max_score

    // Output
    std::string output = "data/training.binpack";  // Output file path (binpack format)
    int flush_interval = 10000;                    // Flush to disk every N positions

    // Opening book
    bool use_book = true;               // Use opening book for variety (default: true)
    std::string book_path = "book/Perfect2023.bin"; // Path to Polyglot opening book
    int book_depth = 12;                // Maximum depth to use book moves (half-moves)
    int random_multi_pv = 0;            // Use multi-PV for randomization (0 = disabled)
};

// ============================================================================
// Statistics
// ============================================================================

struct DataGenStats {
    std::atomic<uint64_t> games_started{0};     // Games reserved/started by threads
    std::atomic<uint64_t> games_completed{0};   // Games fully completed
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

    // Control
    void run();             // Start data generation (blocking)
    void stop();            // Request stop
    bool is_running() const { return running; }

    // Statistics
    const DataGenStats& stats() const { return m_stats; }

private:
    DataGenConfig m_config;
    DataGenStats m_stats;

    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};

    // Output file
    std::ofstream m_output;         // Binpack output
    std::mutex m_output_mutex;

    // Thread workers
    void worker_thread(int thread_id);

    // Game playing
    GameResult play_game(std::vector<TrainingEntry>& entries, int thread_id);

    // Position encoding
    TrainingEntry encode_position(const Board& board, int score, GameResult result);

    // Move selection
    Move select_random_move(Board& board, int thread_id);
    Move select_search_move(Board& board, int& score, int thread_id);

    // Filtering
    bool should_record_position(const Board& board, int score, int ply);

    // Output
    void write_entries(const std::vector<TrainingEntry>& entries);
    void flush_output();

    // Random number generation per thread
    std::vector<uint64_t> m_random_seeds;
    uint64_t rand_next(int thread_id);
    int rand_int(int thread_id, int max);

    // Per-thread searchers for true multi-threading
    std::vector<std::unique_ptr<Search>> m_searchers;
};

// ============================================================================
// Marlinformat Conversion (for Bullet trainer compatibility)
// ============================================================================

// Convert TrainingEntry to Marlinformat binary
void to_marlinformat(const TrainingEntry& entry, std::vector<uint8_t>& output);

// ============================================================================
// Global Interface (used by UCI)
// ============================================================================

// Start data generation with given config
void start(const DataGenConfig& config);

// Stop data generation
void stop();

// Check if running
bool is_running();

// Get statistics
const DataGenStats& get_stats();

// Parse config from UCI command stream
DataGenConfig parse_config(std::istringstream& is);

// ============================================================================
// Binpack File Reading & Conversion
// ============================================================================

// Read entries from binpack file
bool read_binpack_file(const std::string& path, std::vector<TrainingEntry>& entries, size_t max_entries = 0);

// View entries from binpack file (print to stdout)
void view_binpack_file(const std::string& path, size_t count = 10, size_t offset = 0);

// Convert binpack file to EPD text format
bool convert_to_epd(const std::string& binpack_path, const std::string& epd_path, size_t max_entries = 0);

// Get file statistics
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

// Decode TrainingEntry to human-readable string
std::string entry_to_string(const TrainingEntry& entry);

// Decode TrainingEntry to FEN string (approximate - pieces only)
std::string entry_to_fen(const TrainingEntry& entry);

} // namespace DataGen

#endif // DATAGEN_HPP
