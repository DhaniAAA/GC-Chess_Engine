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
#include <memory>

namespace DataGen {

static std::unique_ptr<DataGenerator> g_generator = nullptr;
static std::mutex g_mutex;
static std::mutex g_search_mutex;

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

DataGenerator::DataGenerator(const DataGenConfig& config)
    : m_config(config) {

    m_random_seeds.resize(config.threads);
    uint64_t base_seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < config.threads; ++i) {
        m_random_seeds[i] = base_seed ^ (i * 0x9E3779B97F4A7C15ULL);
    }


    m_searchers.reserve(config.threads);
    for (int i = 0; i < config.threads; ++i) {
        auto searcher = std::make_unique<Search>();
        searcher->set_silent(true);
        m_searchers.push_back(std::move(searcher));
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

    auto create_output_dir = [](const std::string& filepath) {
        std::filesystem::path output_path(filepath);
        std::filesystem::path output_dir = output_path.parent_path();
        if (!output_dir.empty() && !std::filesystem::exists(output_dir)) {
            std::filesystem::create_directories(output_dir);
        }
    };

    create_output_dir(m_config.output);
    m_output.open(m_config.output, std::ios::binary | std::ios::app);
    if (!m_output.is_open()) {
        std::cerr << "Error: Cannot open output file " << m_config.output << std::endl;
        running = false;
        return;
    }

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
    std::cout << "QSearch margin: " << m_config.qsearch_margin << " cp" << std::endl;
    std::cout << "Search margin : " << m_config.search_margin << " cp" << std::endl;
    std::cout << "Max score     : " << m_config.max_score << " cp" << std::endl;
    std::cout << "Eval limit    : " << (m_config.eval_limit > 0 ? std::to_string(m_config.eval_limit) + " cp" : "disabled") << std::endl;
    std::cout << "Format        : binpack" << std::endl;
    std::cout << "Output        : " << m_config.output << std::endl;
    std::cout << "================================\n" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < m_config.threads; ++i) {
        threads.emplace_back(&DataGenerator::worker_thread, this, i);
    }

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

    for (auto& t : threads) {
        t.join();
    }

    running = false;
    progress_thread.join();

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
        uint64_t game_num = m_stats.games_started.fetch_add(1);
        if (game_num >= static_cast<uint64_t>(m_config.games)) {
            break;
        }

        local_entries.clear();
        GameResult result = play_game(local_entries, thread_id);

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
                    entry.result = 1;
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

    if (m_config.use_book && Book::book.is_loaded()) {
        while (ply < m_config.book_depth && state_idx < 510) {
            Move book_move = Book::book.probe(board);
            if (book_move == MOVE_NONE) {
                break;
            }

            board.do_move(book_move, state_stack[state_idx++]);
            ply++;
        }
    }

    int random_moves_made = 0;
    while (random_moves_made < m_config.random_plies && state_idx < 510) {
        Move m = select_random_move(board, thread_id);
        if (m == MOVE_NONE) {
            if (board.in_check()) {
                return board.side_to_move() == WHITE ? GameResult::black_wins : GameResult::white_wins;
            }
            return GameResult::draw;
        }

        board.do_move(m, state_stack[state_idx++]);
        ply++;
        random_moves_made++;
    }

    while (ply < m_config.max_ply && state_idx < 510) {
        MoveList moves;
        MoveGen::generate_legal(board, moves);

        if (moves.size() == 0) {
            if (board.in_check()) {
                return board.side_to_move() == WHITE ? GameResult::black_wins : GameResult::white_wins;
            }
            return GameResult::draw;
        }

        if (board.is_draw(ply)) {
            return GameResult::draw;
        }

        int static_eval;
        Move best_move = select_search_move(board, static_eval, thread_id);

        if (best_move == MOVE_NONE) {
            best_move = moves[0].move;
            static_eval = 0;
        }

        if (should_record_position(board, static_eval, static_eval, ply, best_move, thread_id)) {
            entries.push_back(encode_position(board, static_eval, GameResult::ongoing));
            m_stats.positions_generated++;
        } else {
            m_stats.positions_filtered++;
        }

        int abs_score = std::abs(static_eval);

        if (abs_score >= m_config.adjudicate_score) {
            adjudicate_count++;
            if (adjudicate_count >= m_config.adjudicate_count) {
                return static_eval > 0 ? GameResult::white_wins : GameResult::black_wins;
            }
        } else {
            adjudicate_count = 0;
        }

        if (ply >= m_config.adjudicate_draw_ply && abs_score < m_config.adjudicate_draw) {
            draw_count++;
            if (draw_count >= m_config.adjudicate_draw_count) {
                return GameResult::draw;
            }
        } else {
            draw_count = 0;
        }

        board.do_move(best_move, state_stack[state_idx++]);
        ply++;
    }

    m_stats.total_plies += ply;

    return GameResult::draw;
}

Move DataGenerator::select_random_move(Board& board, int thread_id) {
    MoveList moves;
    MoveGen::generate_legal(board, moves);

    if (moves.size() == 0) {
        return MOVE_NONE;
    }

    int idx = rand_int(thread_id, moves.size());
    return moves[idx].move;
}

Move DataGenerator::select_search_move(Board& board, int& score, int thread_id) {
    SearchLimits limits;
    limits.depth = m_config.depth;
    if (m_config.nodes > 0) {
        limits.nodes = m_config.nodes;
    } else if (m_config.soft_nodes > 0) {
        limits.nodes = m_config.soft_nodes;
    }

    Search& searcher = *m_searchers[thread_id];
    searcher.start(board, limits);

    score = searcher.evaluate(board);
    Move best = searcher.best_move();

    return best;
}

bool DataGenerator::should_record_position(Board& board, int static_eval, int search_score, int ply, Move best_move, int thread_id) {
    if (ply < m_config.min_ply) {
        return false;
    }

    if (m_config.skip_in_check && board.in_check()) {
        return false;
    }

    if (std::abs(static_eval) > m_config.max_score) {
        return false;
    }

    if (m_config.skip_captures && best_move != MOVE_NONE && board.is_capture(best_move)) {
        return false;
    }

    if (m_config.skip_tactical_bestmove && best_move != MOVE_NONE) {
        if (board.is_capture(best_move) || best_move.is_promotion()) {
            return false;
        }
    }

    if (m_config.qsearch_margin > 0) {
        Search& searcher = *m_searchers[thread_id];
        int qsearch_score = searcher.qsearch_score(board);

        int qsearch_diff = std::abs(static_eval - qsearch_score);
        if (qsearch_diff > m_config.qsearch_margin) {
            return false;
        }
    }

    if (m_config.search_margin > 0) {
        int search_diff = std::abs(static_eval - search_score);
        if (search_diff > m_config.search_margin) {
            return false;
        }
    }

    return true;
}

TrainingEntry DataGenerator::encode_position(const Board& board, int score, GameResult result) {
    TrainingEntry entry;
    memset(&entry, 0, sizeof(entry));

    memset(entry.packed_board, 0, sizeof(entry.packed_board));

    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
        Piece pc = board.piece_on(sq);

        uint8_t piece_code = PTYPE_EMPTY;
        if (pc != NO_PIECE) {
            PieceType pt = type_of(pc);
            Color c = color_of(pc);
            piece_code = static_cast<uint8_t>(pt - PAWN + 1);
            if (c == BLACK) {
                piece_code += 6;
            }
        }

        int sq_idx = static_cast<int>(sq);
        int byte_idx = sq_idx / 2;
        if (sq_idx % 2 == 0) {
            entry.packed_board[byte_idx] = (entry.packed_board[byte_idx] & 0xF0) | piece_code;
        } else {
            entry.packed_board[byte_idx] = (entry.packed_board[byte_idx] & 0x0F) | (piece_code << 4);
        }
    }

    entry.stm = board.side_to_move() == WHITE ? 0 : 1;

    entry.castling = static_cast<uint8_t>(board.castling_rights());

    Square ep = board.en_passant_square();
    entry.ep_square = (ep == SQ_NONE) ? 64 : static_cast<uint8_t>(ep);

    entry.rule50 = static_cast<uint8_t>(std::min(255, board.halfmove_clock()));

    switch (result) {
        case GameResult::white_wins: entry.result = 2; break;
        case GameResult::black_wins: entry.result = 0; break;
        case GameResult::draw: entry.result = 1; break;
        default: entry.result = 1; break;
    }

    entry.padding = 0;

    int clamped_score = std::clamp(score, -32000, 32000);
    if (m_config.eval_limit > 0) {
        clamped_score = std::clamp(clamped_score, -m_config.eval_limit, m_config.eval_limit);
    }
    entry.score = static_cast<int16_t>(clamped_score);

    return entry;
}

void DataGenerator::write_entries(const std::vector<TrainingEntry>& entries) {
    std::lock_guard<std::mutex> lock(m_output_mutex);

    if (m_output.is_open()) {
        for (const auto& entry : entries) {
            m_output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        }
    }

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

void start(const DataGenConfig& config) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_generator && g_generator->is_running()) {
        std::cout << "Data generation already running!" << std::endl;
        return;
    }

    g_generator = std::make_unique<DataGenerator>(config);

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
        } else if (token == "eval_limit" || token == "evallimit") {
            is >> config.eval_limit;
        } else if (token == "qsearch" || token == "qsearch_margin") {
            is >> config.qsearch_margin;
        } else if (token == "search_margin") {
            is >> config.search_margin;
        } else if (token == "max_score" || token == "maxscore") {
            is >> config.max_score;
        } else if (token == "no_check_filter") {
            config.skip_in_check = false;
        } else if (token == "no_tactical_filter") {
            config.skip_tactical_bestmove = false;
        }
    }

    config.threads = std::max(1, std::min(config.threads, 128));
    config.depth = std::max(1, std::min(config.depth, 30));
    config.games = std::max(1, config.games);
    config.random_plies = std::max(0, std::min(config.random_plies, 20));
    config.book_depth = std::max(0, std::min(config.book_depth, 30));
    config.hash_mb = std::max(1, std::min(config.hash_mb, 32768));

    return config;
}

std::string entry_to_string(const TrainingEntry& entry) {
    std::ostringstream ss;

    const char* result_str = (entry.result == 2) ? "1-0" :
                             (entry.result == 0) ? "0-1" : "1/2-1/2";

    const char* stm_str = (entry.stm == 0) ? "White" : "Black";

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
    char board[64];
    memset(board, '.', 64);

    static const char piece_chars[] = ".PNBRQKpnbrqk";

    for (int sq = 0; sq < 64; ++sq) {
        int byte_idx = sq / 2;
        uint8_t piece_code;

        if (sq % 2 == 0) {
            piece_code = entry.packed_board[byte_idx] & 0x0F;
        } else {
            piece_code = (entry.packed_board[byte_idx] >> 4) & 0x0F;
        }

        if (piece_code >= 1 && piece_code <= 12) {
            board[sq] = piece_chars[piece_code];
        }
    }

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

    fen << ' ' << (entry.stm == 0 ? 'w' : 'b');

    fen << ' ';
    bool any_castling = false;
    if (entry.castling & 1) { fen << 'K'; any_castling = true; }
    if (entry.castling & 2) { fen << 'Q'; any_castling = true; }
    if (entry.castling & 4) { fen << 'k'; any_castling = true; }
    if (entry.castling & 8) { fen << 'q'; any_castling = true; }
    if (!any_castling) fen << '-';

    fen << ' ';
    if (entry.ep_square < 64) {
        char file_char = 'a' + (entry.ep_square % 8);
        char rank_char = '1' + (entry.ep_square / 8);
        fen << file_char << rank_char;
    } else {
        fen << '-';
    }

    fen << ' ' << static_cast<int>(entry.rule50) << " 1";

    return fen.str();
}

bool read_binpack_file(const std::string& path, std::vector<TrainingEntry>& entries, size_t max_entries) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << path << std::endl;
        return false;
    }

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

    in_file.seekg(0, std::ios::end);
    size_t file_size = in_file.tellg();
    in_file.seekg(0, std::ios::beg);
    size_t total_entries = file_size / sizeof(TrainingEntry);

    size_t entries_to_convert = max_entries > 0 ? std::min(max_entries, total_entries) : total_entries;

    std::cout << "Converting " << entries_to_convert << " entries to EPD format..." << std::endl;

    size_t count = 0;
    TrainingEntry entry;
    while (count < entries_to_convert && in_file.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        std::string fen = entry_to_fen(entry);
        std::istringstream fen_stream(fen);
        std::string field;
        for (int i = 0; i < 4 && fen_stream >> field; ++i) {
            if (i > 0) out_file << ' ';
            out_file << field;
        }

        const char* result_str = (entry.result == 2) ? "1-0" :
                                 (entry.result == 0) ? "0-1" : "1/2-1/2";
        out_file << " c9 \"" << result_str << "\";";
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

bool entry_to_board(const TrainingEntry& entry, Board& board, StateInfo& si) {
    std::string fen = entry_to_fen(entry);
    try {
        board.set(fen, &si);
        return true;
    } catch (...) {
        return false;
    }
}

bool filter_binpack(const FilterConfig& config, FilterStats& stats) {

    std::ifstream input(config.input_path, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Error: Cannot open input file " << config.input_path << std::endl;
        return false;
    }


    std::ofstream output(config.output_path, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "Error: Cannot open output file " << config.output_path << std::endl;
        return false;
    }


    input.seekg(0, std::ios::end);
    size_t file_size = input.tellg();
    input.seekg(0, std::ios::beg);
    size_t total_entries = file_size / sizeof(TrainingEntry);

    std::cout << "\n=== Filtering Training Data ===" << std::endl;
    std::cout << "Input       : " << config.input_path << std::endl;
    std::cout << "Output      : " << config.output_path << std::endl;
    std::cout << "Total entries: " << total_entries << std::endl;
    std::cout << "Filters:" << std::endl;
    std::cout << "  skip_in_check  : " << (config.skip_in_check ? "true" : "false") << std::endl;
    std::cout << "  qsearch_margin : " << config.qsearch_margin << " cp" << std::endl;
    std::cout << "  max_score      : " << config.max_score << " cp" << std::endl;
    std::cout << "  eval_limit     : " << (config.eval_limit > 0 ? std::to_string(config.eval_limit) + " cp" : "disabled") << std::endl;
    std::cout << "==============================" << std::endl;

    std::unique_ptr<Search> searcher = std::make_unique<Search>();

    stats = FilterStats{};
    auto start_time = std::chrono::steady_clock::now();

    TrainingEntry entry;
    StateInfo si;
    Board board;


    try {
        while (input.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
            stats.total_read++;

            if (!entry_to_board(entry, board, si)) {
                continue;
            }

            int stored_score = entry.score;

            if (config.skip_in_check && board.in_check()) {
                stats.filtered_check++;
                continue;
            }

            if (std::abs(stored_score) > config.max_score) {
                stats.filtered_score++;
                continue;
            }

            if (config.qsearch_margin > 0) {
                int static_eval = searcher->evaluate(board);

                int qsearch_score = searcher->qsearch_score(board);
                int qsearch_diff = std::abs(static_eval - qsearch_score);
                if (qsearch_diff > config.qsearch_margin) {
                    stats.filtered_qsearch++;
                    continue;
                }
            }

            TrainingEntry clamped_entry = entry;
            if (config.eval_limit > 0) {
                int16_t clamped_score = static_cast<int16_t>(
                    std::clamp(static_cast<int>(entry.score), -config.eval_limit, config.eval_limit)
                );
                if (clamped_score != entry.score) {
                    clamped_entry.score = clamped_score;
                    stats.clamped_eval_limit++;
                }
            }

            output.write(reinterpret_cast<const char*>(&clamped_entry), sizeof(clamped_entry));
            stats.passed++;

            if (stats.total_read % config.report_interval == 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                double pct = 100.0 * stats.total_read / total_entries;
                double pass_rate = 100.0 * stats.passed / stats.total_read;

                std::cout << "Progress: " << stats.total_read << "/" << total_entries
                          << " (" << std::fixed << std::setprecision(1) << pct << "%)"
                          << " | Passed: " << stats.passed
                          << " (" << std::setprecision(1) << pass_rate << "%)"
                          << " | Time: " << elapsed << "s"
                          << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error during filtering at entry " << stats.total_read << ": " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown error during filtering at entry " << stats.total_read << std::endl;
        return false;
    }

    output.flush();
    output.close();
    input.close();

    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

    std::cout << "\n=== Filter Complete ===" << std::endl;
    std::cout << "Total time     : " << total_time << " seconds" << std::endl;
    std::cout << "Positions read : " << stats.total_read << std::endl;
    std::cout << "Positions passed: " << stats.passed
              << " (" << std::setprecision(1) << (100.0 * stats.passed / stats.total_read) << "%)" << std::endl;
    std::cout << "Filtered by:" << std::endl;
    std::cout << "  In-check     : " << stats.filtered_check << std::endl;
    std::cout << "  Extreme score: " << stats.filtered_score << std::endl;
    std::cout << "  QSearch      : " << stats.filtered_qsearch << std::endl;
    if (config.eval_limit > 0) {
        std::cout << "Clamped by eval_limit: " << stats.clamped_eval_limit << std::endl;
    }
    std::cout << "Output saved to: " << config.output_path << std::endl;
    std::cout << "========================" << std::endl;

    return true;
}

FilterConfig parse_filter_config(std::istringstream& is) {
    FilterConfig config;
    std::string token;

    while (is >> token) {
        if (token == "input") {
            is >> config.input_path;
        } else if (token == "output") {
            is >> config.output_path;
        } else if (token == "threads") {
            is >> config.threads;
        } else if (token == "on" || token == "qsearch") {
            is >> config.qsearch_margin;
        } else if (token == "max_score" || token == "maxscore") {
            is >> config.max_score;
        } else if (token == "eval_limit" || token == "evallimit") {
            is >> config.eval_limit;
        } else if (token == "no_check_filter") {
            config.skip_in_check = false;
        } else if (token == "no_tactical_filter") {
            config.skip_tactical_bestmove = false;
        }
    }

    if (config.output_path.empty() && !config.input_path.empty()) {
        size_t dot_pos = config.input_path.rfind('.');
        if (dot_pos != std::string::npos) {
            config.output_path = config.input_path.substr(0, dot_pos) + "_filtered.binpack";
        } else {
            config.output_path = config.input_path + "_filtered.binpack";
        }
    }

    return config;
}

}