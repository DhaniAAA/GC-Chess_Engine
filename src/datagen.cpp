// ============================================================================
// datagen.cpp - Data Generation for NNUE Training
// ============================================================================

#include "datagen.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include "search.hpp"
#include "book.hpp"
#include "tt.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <filesystem>

namespace DataGen {

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<DataGenerator> g_generator = nullptr;
static std::mutex g_mutex;
static std::mutex g_search_mutex;  // Protects global Searcher for multi-threaded datagen

// ============================================================================
// Statistics Printing
// ============================================================================

void DataGenStats::print() const {
    uint64_t games = games_completed.load();
    uint64_t pos = positions_generated.load();
    uint64_t filtered = positions_filtered.load();
    uint64_t plies = total_plies.load();

    std::cout << "\n=== Data Generation Statistics ===" << std::endl;
    std::cout << "Games completed  : " << games << std::endl;
    std::cout << "  White wins     : " << games_white_wins.load()
              << " (" << (games > 0 ? games_white_wins.load() * 100.0 / games : 0) << "%)" << std::endl;
    std::cout << "  Black wins     : " << games_black_wins.load()
              << " (" << (games > 0 ? games_black_wins.load() * 100.0 / games : 0) << "%)" << std::endl;
    std::cout << "  Draws          : " << games_draws.load()
              << " (" << (games > 0 ? games_draws.load() * 100.0 / games : 0) << "%)" << std::endl;
    std::cout << "Positions saved  : " << pos << std::endl;
    std::cout << "Positions filtered: " << filtered << std::endl;
    std::cout << "Avg plies/game   : " << (games > 0 ? plies / games : 0) << std::endl;
    std::cout << "Avg pos/game     : " << (games > 0 ? pos / games : 0) << std::endl;
    std::cout << "==================================" << std::endl;
}

// ============================================================================
// Data Generator Implementation
// ============================================================================

DataGenerator::DataGenerator(const DataGenConfig& config)
    : m_config(config) {

    // Initialize random seeds for each thread
    m_random_seeds.resize(config.threads);
    uint64_t base_seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < config.threads; ++i) {
        m_random_seeds[i] = base_seed ^ (i * 0x9E3779B97F4A7C15ULL);
    }

    // Create per-thread searchers for true multi-threading
    m_searchers.reserve(config.threads);
    for (int i = 0; i < config.threads; ++i) {
        m_searchers.push_back(std::make_unique<Search>());
    }
}

DataGenerator::~DataGenerator() {
    stop();
    if (m_output.is_open()) {
        m_output.close();
    }
}

void DataGenerator::run() {
    if (running.load()) {
        std::cout << "Data generation already running!" << std::endl;
        return;
    }

    running = true;
    stop_requested = false;
    m_stats.reset();

    // Create output directory if needed
    auto create_output_dir = [](const std::string& filepath) {
        std::filesystem::path output_path(filepath);
        std::filesystem::path output_dir = output_path.parent_path();
        if (!output_dir.empty() && !std::filesystem::exists(output_dir)) {
            std::filesystem::create_directories(output_dir);
        }
    };

    // Open binpack output file
    create_output_dir(m_config.output);
    m_output.open(m_config.output, std::ios::binary | std::ios::app);
    if (!m_output.is_open()) {
        std::cerr << "Error: Cannot open output file " << m_config.output << std::endl;
        running = false;
        return;
    }

    // Load opening book if requested
    bool book_loaded = false;
    if (m_config.use_book) {
        if (!Book::book.is_loaded()) {
            book_loaded = Book::book.load(m_config.book_path);
            if (!book_loaded) {
                std::cerr << "Warning: Could not load opening book: " << m_config.book_path << std::endl;
                std::cerr << "         Falling back to random openings." << std::endl;
            }
        } else {
            book_loaded = true;
        }
    }

    // Resize hash table for datagen
    TT.resize(m_config.hash_mb);
    TT.clear();

    std::cout << "\n=== Starting Data Generation ===" << std::endl;
    std::cout << "Threads       : " << m_config.threads << std::endl;
    std::cout << "Hash          : " << m_config.hash_mb << " MB" << std::endl;
    std::cout << "Depth         : " << m_config.depth << std::endl;
    std::cout << "Games target  : " << m_config.games << std::endl;
    std::cout << "Opening book  : " << (m_config.use_book && book_loaded ?
                                        m_config.book_path + " (loaded, depth " + std::to_string(m_config.book_depth) + ")" :
                                        "disabled") << std::endl;
    std::cout << "Random plies  : " << m_config.random_plies << std::endl;
    std::cout << "Format        : binpack" << std::endl;
    std::cout << "Output        : " << m_config.output << std::endl;
    std::cout << "================================\n" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    // Launch worker threads
    std::vector<std::thread> threads;
    for (int i = 0; i < m_config.threads; ++i) {
        threads.emplace_back(&DataGenerator::worker_thread, this, i);
    }

    // Progress reporting thread
    std::thread progress_thread([this, start_time]() {
        while (running.load() && !stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

            uint64_t games = m_stats.games_completed.load();
            uint64_t pos = m_stats.positions_generated.load();

            double games_per_sec = elapsed > 0 ? static_cast<double>(games) / elapsed : 0;
            double pos_per_sec = elapsed > 0 ? static_cast<double>(pos) / elapsed : 0;

            std::cout << "Progress: " << games << "/" << m_config.games << " games"
                      << " | " << pos << " positions"
                      << " | " << std::fixed << std::setprecision(1) << games_per_sec << " games/s"
                      << " | " << std::setprecision(0) << pos_per_sec << " pos/s"
                      << " | Time: " << elapsed << "s"
                      << std::endl;
        }
    });

    // Wait for all workers
    for (auto& t : threads) {
        t.join();
    }

    running = false;
    progress_thread.join();

    // Final flush and stats
    flush_output();
    if (m_output.is_open()) m_output.close();

    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

    std::cout << "\n=== Data Generation Complete ===" << std::endl;
    std::cout << "Total time: " << total_time << " seconds" << std::endl;
    m_stats.print();
}

void DataGenerator::stop() {
    stop_requested = true;
}

void DataGenerator::worker_thread(int thread_id) {
    std::vector<TrainingEntry> local_entries;
    local_entries.reserve(1000);

    while (!stop_requested.load()) {
        // Atomically reserve a game slot BEFORE playing
        // This prevents multiple threads from starting more games than requested
        uint64_t game_num = m_stats.games_started.fetch_add(1);
        if (game_num >= static_cast<uint64_t>(m_config.games)) {
            // We've already started enough games, exit
            break;
        }

        // Play a game
        local_entries.clear();
        GameResult result = play_game(local_entries, thread_id);

        // Update game result statistics
        m_stats.games_completed++;
        switch (result) {
            case GameResult::white_wins:
                m_stats.games_white_wins++;
                break;
            case GameResult::black_wins:
                m_stats.games_black_wins++;
                break;
            case GameResult::draw:
                m_stats.games_draws++;
                break;
            default:
                break;
        }

        // Update entries with final result and write
        for (auto& entry : local_entries) {
            switch (result) {
                case GameResult::white_wins:
                    entry.result = 2;
                    break;
                case GameResult::black_wins:
                    entry.result = 0;
                    break;
                case GameResult::draw:
                    entry.result = 1;
                    break;
                default:
                    entry.result = 1;  // Default to draw if ongoing
                    break;
            }
        }

        if (!local_entries.empty()) {
            write_entries(local_entries);
        }
    }
}

GameResult DataGenerator::play_game(std::vector<TrainingEntry>& entries, int thread_id) {
    StateInfo state_stack[512];
    int state_idx = 0;

    Board board;
    board.set(Board::StartFEN, &state_stack[state_idx++]);

    int ply = 0;
    int adjudicate_count = 0;
    int draw_count = 0;

    // =========================================================================
    // Phase 1: Opening book phase
    // =========================================================================
    if (m_config.use_book && Book::book.is_loaded()) {
        while (ply < m_config.book_depth && state_idx < 510) {
            Move book_move = Book::book.probe(board);
            if (book_move == MOVE_NONE) {
                break;  // No more book moves, proceed to random/search phase
            }

            board.do_move(book_move, state_stack[state_idx++]);
            ply++;
        }
    }

    // =========================================================================
    // Phase 2: Random opening phase (after book, for variety)
    // =========================================================================
    int random_moves_made = 0;
    while (random_moves_made < m_config.random_plies && state_idx < 510) {
        Move m = select_random_move(board, thread_id);
        if (m == MOVE_NONE) {
            // No legal moves - game over
            if (board.in_check()) {
                return board.side_to_move() == WHITE ? GameResult::black_wins : GameResult::white_wins;
            }
            return GameResult::draw;
        }

        board.do_move(m, state_stack[state_idx++]);
        ply++;
        random_moves_made++;
    }

    // =========================================================================
    // Phase 3: Main game phase (search-based play)
    // =========================================================================
    while (ply < m_config.max_ply && state_idx < 510) {
        // Check for terminal positions
        MoveList moves;
        MoveGen::generate_legal(board, moves);

        if (moves.size() == 0) {
            // Checkmate or stalemate
            if (board.in_check()) {
                return board.side_to_move() == WHITE ? GameResult::black_wins : GameResult::white_wins;
            }
            return GameResult::draw;
        }

        // Check for draw by 50-move rule, 3-fold repetition, or insufficient material
        if (board.is_draw(ply)) {
            return GameResult::draw;
        }

        // Search for best move
        int score;
        Move best_move = select_search_move(board, score, thread_id);

        if (best_move == MOVE_NONE) {
            // Fallback to first legal move
            best_move = moves[0].move;
            score = 0;
        }

        // Record position if appropriate
        if (should_record_position(board, score, ply)) {
            entries.push_back(encode_position(board, score, GameResult::ongoing));
            m_stats.positions_generated++;
        } else {
            m_stats.positions_filtered++;
        }

        // Adjudication check
        int abs_score = std::abs(score);

        // Resignation adjudication: one side is winning decisively
        if (abs_score >= m_config.adjudicate_score) {
            adjudicate_count++;
            if (adjudicate_count >= m_config.adjudicate_count) {
                // Score is from perspective of side to move
                return score > 0 ?
                    (board.side_to_move() == WHITE ? GameResult::white_wins : GameResult::black_wins) :
                    (board.side_to_move() == WHITE ? GameResult::black_wins : GameResult::white_wins);
            }
        } else {
            adjudicate_count = 0;
        }

        // Draw adjudication: position is very equal for extended period
        // Only after sufficient ply to avoid adjudicating early games
        if (ply >= m_config.adjudicate_draw_ply && abs_score < m_config.adjudicate_draw) {
            draw_count++;
            if (draw_count >= m_config.adjudicate_draw_count) {
                return GameResult::draw;
            }
        } else {
            draw_count = 0;
        }

        // Make move
        board.do_move(best_move, state_stack[state_idx++]);
        ply++;
    }

    m_stats.total_plies += ply;

    // Game exceeded max length - adjudicate as draw
    return GameResult::draw;
}

Move DataGenerator::select_random_move(Board& board, int thread_id) {
    MoveList moves;
    MoveGen::generate_legal(board, moves);

    if (moves.size() == 0) {
        return MOVE_NONE;
    }

    // Select random move
    int idx = rand_int(thread_id, moves.size());
    return moves[idx].move;
}

Move DataGenerator::select_search_move(Board& board, int& score, int thread_id) {
    // Create a mini search for this position
    // Each thread has its own searcher for true multi-threading

    SearchLimits limits;
    limits.depth = m_config.depth;
    if (m_config.nodes > 0) {
        limits.nodes = m_config.nodes;
    } else if (m_config.soft_nodes > 0) {
        limits.nodes = m_config.soft_nodes;
    }

    // Use thread-local searcher for true parallelism
    Search& searcher = *m_searchers[thread_id];

    // Run search synchronously
    searcher.start(board, limits);

    // Get the static evaluation for training data
    // The search score is relative to side to move, so we use evaluate for consistency
    score = searcher.evaluate(board);
    Move best = searcher.best_move();

    // Score is already from white's perspective in evaluate()
    // No adjustment needed

    return best;
}

bool DataGenerator::should_record_position(const Board& board, int score, int ply) {
    // Skip early game positions
    if (ply < m_config.min_ply) {
        return false;
    }

    // Skip positions in check
    if (m_config.skip_in_check && board.in_check()) {
        return false;
    }

    // Skip extreme scores
    if (std::abs(score) > m_config.max_score) {
        return false;
    }

    return true;
}

TrainingEntry DataGenerator::encode_position(const Board& board, int score, GameResult result) {
    TrainingEntry entry;
    memset(&entry, 0, sizeof(entry));

    // Initialize packed_board with empty (0x00 = all nibbles are PTYPE_EMPTY)
    memset(entry.packed_board, 0, sizeof(entry.packed_board));

    // Encode all 64 squares using packed format
    // Each byte stores 2 squares: lower nibble = even sq, upper nibble = odd sq
    // Piece encoding: 0=empty, 1-6=White(P,N,B,R,Q,K), 7-12=Black(P,N,B,R,Q,K)
    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
        Piece pc = board.piece_on(sq);

        uint8_t piece_code = PTYPE_EMPTY;  // 0 = empty
        if (pc != NO_PIECE) {
            PieceType pt = type_of(pc);
            Color c = color_of(pc);
            // piece_code: WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6, BP=7, BN=8, BB=9, BR=10, BQ=11, BK=12
            piece_code = static_cast<uint8_t>(pt - PAWN + 1);  // 1-6 for P,N,B,R,Q,K
            if (c == BLACK) {
                piece_code += 6;  // 7-12 for Black pieces
            }
        }

        // Store in packed_board
        int sq_idx = static_cast<int>(sq);
        int byte_idx = sq_idx / 2;
        if (sq_idx % 2 == 0) {
            // Even square: lower nibble
            entry.packed_board[byte_idx] = (entry.packed_board[byte_idx] & 0xF0) | piece_code;
        } else {
            // Odd square: upper nibble
            entry.packed_board[byte_idx] = (entry.packed_board[byte_idx] & 0x0F) | (piece_code << 4);
        }
    }

    // Side to move
    entry.stm = board.side_to_move() == WHITE ? 0 : 1;

    // Castling rights
    entry.castling = static_cast<uint8_t>(board.castling_rights());

    // En passant square
    Square ep = board.en_passant_square();
    entry.ep_square = (ep == SQ_NONE) ? 64 : static_cast<uint8_t>(ep);

    // 50-move rule counter
    entry.rule50 = static_cast<uint8_t>(std::min(255, board.halfmove_clock()));

    // Result (will be updated after game ends)
    switch (result) {
        case GameResult::white_wins: entry.result = 2; break;
        case GameResult::black_wins: entry.result = 0; break;
        case GameResult::draw: entry.result = 1; break;
        default: entry.result = 1; break;  // Ongoing treated as draw placeholder
    }

    // Padding
    entry.padding = 0;

    // Score (from white's perspective, clamped)
    entry.score = static_cast<int16_t>(std::clamp(score, -32000, 32000));

    return entry;
}

void DataGenerator::write_entries(const std::vector<TrainingEntry>& entries) {
    std::lock_guard<std::mutex> lock(m_output_mutex);

    // Write binpack format
    if (m_output.is_open()) {
        for (const auto& entry : entries) {
            m_output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        }
    }

    // Periodic flush
    static std::atomic<uint64_t> write_count{0};
    write_count += entries.size();
    if (write_count >= static_cast<uint64_t>(m_config.flush_interval)) {
        if (m_output.is_open()) m_output.flush();
        write_count = 0;
    }
}

void DataGenerator::flush_output() {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    if (m_output.is_open()) {
        m_output.flush();
    }
}

// Xorshift64 random number generator
uint64_t DataGenerator::rand_next(int thread_id) {
    uint64_t x = m_random_seeds[thread_id];
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m_random_seeds[thread_id] = x;
    return x;
}

int DataGenerator::rand_int(int thread_id, int max) {
    if (max <= 0) return 0;
    return static_cast<int>(rand_next(thread_id) % static_cast<uint64_t>(max));
}

// ============================================================================\n// Global Interface\n// ============================================================================

void start(const DataGenConfig& config) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_generator && g_generator->is_running()) {
        std::cout << "Data generation already running!" << std::endl;
        return;
    }

    g_generator = std::make_unique<DataGenerator>(config);

    // Run in background thread
    std::thread([config]() {
        g_generator->run();
    }).detach();
}

void stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_generator) {
        g_generator->stop();
    }
}

bool is_running() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_generator && g_generator->is_running();
}

const DataGenStats& get_stats() {
    static DataGenStats empty_stats;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_generator) {
        return g_generator->stats();
    }
    return empty_stats;
}

DataGenConfig parse_config(std::istringstream& is) {
    DataGenConfig config;
    std::string token;

    while (is >> token) {
        if (token == "threads") {
            is >> config.threads;
        } else if (token == "depth") {
            is >> config.depth;
        } else if (token == "nodes") {
            is >> config.nodes;
        } else if (token == "games") {
            is >> config.games;
        } else if (token == "random") {
            is >> config.random_plies;
        } else if (token == "maxply") {
            is >> config.max_ply;
        } else if (token == "output") {
            is >> config.output;
        } else if (token == "resign") {
            is >> config.adjudicate_score;
        } else if (token == "drawscore") {
            is >> config.adjudicate_draw;
        } else if (token == "book") {
            is >> config.book_path;
            config.use_book = true;
        } else if (token == "bookdepth") {
            is >> config.book_depth;
        } else if (token == "nobook") {
            config.use_book = false;
        } else if (token == "hash") {
            is >> config.hash_mb;
        }
    }

    // Clamp values
    config.threads = std::max(1, std::min(config.threads, 128));
    config.depth = std::max(1, std::min(config.depth, 30));
    config.games = std::max(1, config.games);
    config.random_plies = std::max(0, std::min(config.random_plies, 20));
    config.book_depth = std::max(0, std::min(config.book_depth, 30));
    config.hash_mb = std::max(1, std::min(config.hash_mb, 32768));

    return config;
}

// ============================================================================
// Binpack File Reading & Conversion Implementation
// ============================================================================

std::string entry_to_string(const TrainingEntry& entry) {
    std::ostringstream ss;

    // Result
    const char* result_str = (entry.result == 2) ? "1-0" :
                             (entry.result == 0) ? "0-1" : "1/2-1/2";

    // Side to move
    const char* stm_str = (entry.stm == 0) ? "White" : "Black";

    // En passant
    std::string ep_str = "-";
    if (entry.ep_square < 64) {
        char file_char = 'a' + (entry.ep_square % 8);
        char rank_char = '1' + (entry.ep_square / 8);
        ep_str = std::string(1, file_char) + std::string(1, rank_char);
    }

    ss << "Score: " << entry.score << " cp"
       << " | Result: " << result_str
       << " | STM: " << stm_str
       << " | Castling: " << static_cast<int>(entry.castling)
       << " | EP: " << ep_str
       << " | Rule50: " << static_cast<int>(entry.rule50);

    return ss.str();
}

std::string entry_to_fen(const TrainingEntry& entry) {
    // Reconstruct FEN from TrainingEntry with full 64-square packed board
    char board[64];
    memset(board, '.', 64);

    // Piece character lookup table (indexed by piece_code 1-12)
    // 0=empty, 1=WP, 2=WN, 3=WB, 4=WR, 5=WQ, 6=WK, 7=BP, 8=BN, 9=BB, 10=BR, 11=BQ, 12=BK
    static const char piece_chars[] = ".PNBRQKpnbrqk";

    // Decode all 64 squares from packed_board
    for (int sq = 0; sq < 64; ++sq) {
        int byte_idx = sq / 2;
        uint8_t piece_code;

        if (sq % 2 == 0) {
            // Even square: lower nibble
            piece_code = entry.packed_board[byte_idx] & 0x0F;
        } else {
            // Odd square: upper nibble
            piece_code = (entry.packed_board[byte_idx] >> 4) & 0x0F;
        }

        if (piece_code >= 1 && piece_code <= 12) {
            board[sq] = piece_chars[piece_code];
        }
        // piece_code == 0 means empty, already handled by initial memset
    }

    // Build FEN string
    std::ostringstream fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty_count = 0;
        for (int file = 0; file < 8; ++file) {
            int sq = rank * 8 + file;
            if (board[sq] == '.') {
                empty_count++;
            } else {
                if (empty_count > 0) {
                    fen << empty_count;
                    empty_count = 0;
                }
                fen << board[sq];
            }
        }
        if (empty_count > 0) {
            fen << empty_count;
        }
        if (rank > 0) fen << '/';
    }

    // Side to move
    fen << ' ' << (entry.stm == 0 ? 'w' : 'b');

    // Castling
    fen << ' ';
    bool any_castling = false;
    if (entry.castling & 1) { fen << 'K'; any_castling = true; }
    if (entry.castling & 2) { fen << 'Q'; any_castling = true; }
    if (entry.castling & 4) { fen << 'k'; any_castling = true; }
    if (entry.castling & 8) { fen << 'q'; any_castling = true; }
    if (!any_castling) fen << '-';

    // En passant
    fen << ' ';
    if (entry.ep_square < 64) {
        char file_char = 'a' + (entry.ep_square % 8);
        char rank_char = '1' + (entry.ep_square / 8);
        fen << file_char << rank_char;
    } else {
        fen << '-';
    }

    // Halfmove clock and fullmove number
    fen << ' ' << static_cast<int>(entry.rule50) << " 1";

    return fen.str();
}

bool read_binpack_file(const std::string& path, std::vector<TrainingEntry>& entries, size_t max_entries) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << path << std::endl;
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t total_entries = file_size / sizeof(TrainingEntry);
    size_t entries_to_read = max_entries > 0 ? std::min(max_entries, total_entries) : total_entries;

    entries.resize(entries_to_read);
    file.read(reinterpret_cast<char*>(entries.data()), entries_to_read * sizeof(TrainingEntry));

    return true;
}

void view_binpack_file(const std::string& path, size_t count, size_t offset) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << path << std::endl;
        return;
    }

    // Get file size and entry count
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    size_t total_entries = file_size / sizeof(TrainingEntry);

    std::cout << "\n=== Training Data File: " << path << " ===" << std::endl;
    std::cout << "File size: " << file_size << " bytes" << std::endl;
    std::cout << "Total entries: " << total_entries << std::endl;
    std::cout << "Entry size: " << sizeof(TrainingEntry) << " bytes" << std::endl;
    std::cout << std::endl;

    if (offset >= total_entries) {
        std::cout << "Offset " << offset << " is beyond file end." << std::endl;
        return;
    }

    // Seek to offset
    file.seekg(offset * sizeof(TrainingEntry), std::ios::beg);

    size_t entries_to_show = std::min(count, total_entries - offset);

    std::cout << "Showing entries " << offset << " to " << (offset + entries_to_show - 1) << ":\n" << std::endl;

    for (size_t i = 0; i < entries_to_show; ++i) {
        TrainingEntry entry;
        file.read(reinterpret_cast<char*>(&entry), sizeof(entry));

        std::cout << "[" << (offset + i) << "] " << entry_to_fen(entry) << std::endl;
        std::cout << "    " << entry_to_string(entry) << std::endl;
        std::cout << std::endl;
    }
}

bool convert_to_epd(const std::string& binary_path, const std::string& epd_path, size_t max_entries) {
    std::ifstream in_file(binary_path, std::ios::binary);
    if (!in_file.is_open()) {
        std::cerr << "Error: Cannot open binary file " << binary_path << std::endl;
        return false;
    }

    std::ofstream out_file(epd_path);
    if (!out_file.is_open()) {
        std::cerr << "Error: Cannot create EPD file " << epd_path << std::endl;
        return false;
    }

    // Get total entries
    in_file.seekg(0, std::ios::end);
    size_t file_size = in_file.tellg();
    in_file.seekg(0, std::ios::beg);
    size_t total_entries = file_size / sizeof(TrainingEntry);

    size_t entries_to_convert = max_entries > 0 ? std::min(max_entries, total_entries) : total_entries;

    std::cout << "Converting " << entries_to_convert << " entries to EPD format..." << std::endl;

    size_t count = 0;
    TrainingEntry entry;
    while (count < entries_to_convert && in_file.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        // Write FEN (first 4 fields only for EPD)
        std::string fen = entry_to_fen(entry);
        std::istringstream fen_stream(fen);
        std::string field;
        for (int i = 0; i < 4 && fen_stream >> field; ++i) {
            if (i > 0) out_file << ' ';
            out_file << field;
        }

        // Write result
        const char* result_str = (entry.result == 2) ? "1-0" :
                                 (entry.result == 0) ? "0-1" : "1/2-1/2";
        out_file << " c9 \"" << result_str << "\";";

        // Write score
        out_file << " c0 \"" << entry.score << "\";\n";

        count++;
        if (count % 100000 == 0) {
            std::cout << "  Converted " << count << "/" << entries_to_convert << " entries...\r" << std::flush;
        }
    }

    std::cout << "\nConversion complete! Written to " << epd_path << std::endl;
    return true;
}

bool get_file_stats(const std::string& path, FileStats& stats) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    stats = FileStats{};
    stats.min_score = 32767;
    stats.max_score = -32768;

    TrainingEntry entry;
    while (file.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        stats.total_entries++;

        if (entry.result == 2) stats.white_wins++;
        else if (entry.result == 0) stats.black_wins++;
        else stats.draws++;

        stats.total_score += entry.score;
        stats.min_score = std::min(stats.min_score, static_cast<int>(entry.score));
        stats.max_score = std::max(stats.max_score, static_cast<int>(entry.score));
    }

    return stats.total_entries > 0;
}

} // namespace DataGen
