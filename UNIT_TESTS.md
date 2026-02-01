# GC-Chess Engine - Unit Test Plan

## Overview

Dokumen ini berisi daftar lengkap unit test yang diperlukan untuk memastikan semua fitur engine berfungsi dengan benar.

---

## 1. Types & Basic Data Structures (`types.hpp`)

### 1.1 Color Tests

- [x] `test_color_flip` - Verifikasi `~WHITE == BLACK` dan `~BLACK == WHITE`

### 1.2 Square Tests

- [x] `test_make_square` - Verifikasi `make_square(FILE_A, RANK_1) == SQ_A1`
- [x] `test_file_of` - Verifikasi `file_of(SQ_E4) == FILE_E`
- [x] `test_rank_of` - Verifikasi `rank_of(SQ_E4) == RANK_4`
- [x] `test_relative_rank` - Verifikasi relative rank untuk kedua warna
- [x] `test_relative_square` - Verifikasi square flipping untuk warna

### 1.3 Piece Tests

- [x] `test_make_piece` - Verifikasi pembuatan piece dari color + piece type
- [x] `test_color_of` - Verifikasi ekstraksi warna dari piece
- [x] `test_type_of` - Verifikasi ekstraksi piece type dari piece

### 1.4 CastlingRights Tests

- [x] `test_castling_or` - Verifikasi OR operator untuk castling rights
- [x] `test_castling_and` - Verifikasi AND operator untuk castling rights

### 1.5 EvalScore Tests

- [x] `test_evalscore_addition` - Verifikasi penjumlahan mg/eg scores
- [x] `test_evalscore_subtraction` - Verifikasi pengurangan mg/eg scores
- [x] `test_evalscore_negation` - Verifikasi negasi score
- [x] `test_evalscore_multiplication` - Verifikasi perkalian dengan scalar

---

## 2. Bitboard Operations (`bitboard.hpp`, `bitboard.cpp`)

### 2.1 Basic Bitboard Tests

- [x] `test_square_bb` - Verifikasi konversi square ke bitboard
- [x] `test_bitboard_operators` - Verifikasi &, |, ^ dengan Square
- [x] `test_more_than_one` - Verifikasi deteksi multiple bits

### 2.2 Bit Manipulation Tests

- [x] `test_popcount` - Verifikasi counting bits (0-64 bits)
- [x] `test_lsb` - Verifikasi least significant bit
- [x] `test_msb` - Verifikasi most significant bit
- [x] `test_pop_lsb` - Verifikasi pop dan return lsb

### 2.3 Shift Tests

- [x] `test_shift_north` - Verifikasi shift NORTH
- [x] `test_shift_south` - Verifikasi shift SOUTH
- [x] `test_shift_east` - Verifikasi shift EAST (no wrap)
- [x] `test_shift_west` - Verifikasi shift WEST (no wrap)
- [x] `test_shift_diagonals` - Verifikasi semua diagonal shifts

### 2.4 Attack Table Tests

- [x] `test_pawn_attacks` - Verifikasi pawn attack tables untuk kedua warna
- [x] `test_knight_attacks` - Verifikasi knight attack patterns
- [x] `test_king_attacks` - Verifikasi king attack patterns
- [x] `test_pawn_attacks_bb` - Verifikasi pawn attacks dari bitboard

### 2.5 Line & Between Tests

- [x] `test_between_bb` - Verifikasi squares between dua squares
- [x] `test_line_bb` - Verifikasi line through dua squares
- [x] `test_aligned` - Verifikasi alignment tiga squares

### 2.6 Initialization Tests

- [x] `test_bitboards_init` - Verifikasi inisialisasi semua tables

---

## 3. Magic Bitboards (`magic.hpp`, `magic.cpp`)

### 3.1 Bishop Magic Tests

- [ ] `test_bishop_attacks_empty` - Bishop attacks pada empty board
- [ ] `test_bishop_attacks_blocked` - Bishop attacks dengan blockers
- [ ] `test_bishop_attacks_corners` - Bishop attacks dari corner squares
- [ ] `test_bishop_attacks_center` - Bishop attacks dari center squares

### 3.2 Rook Magic Tests

- [ ] `test_rook_attacks_empty` - Rook attacks pada empty board
- [ ] `test_rook_attacks_blocked` - Rook attacks dengan blockers
- [ ] `test_rook_attacks_corners` - Rook attacks dari corner squares
- [ ] `test_rook_attacks_edges` - Rook attacks dari edge squares

### 3.3 Initialization Tests

- [ ] `test_magics_init` - Verifikasi inisialisasi magic bitboards

---

## 4. Zobrist Hashing (`zobrist.hpp`, `zobrist.cpp`)

### 4.1 Key Generation Tests

- [ ] `test_zobrist_piece_keys` - Verifikasi unique keys untuk piece-square
- [ ] `test_zobrist_castling_keys` - Verifikasi castling right keys
- [ ] `test_zobrist_ep_keys` - Verifikasi en passant file keys
- [ ] `test_zobrist_side_key` - Verifikasi side-to-move key

### 4.2 Key Properties Tests

- [ ] `test_zobrist_symmetry` - XOR dua kali = original
- [ ] `test_zobrist_uniqueness` - Tidak ada collision untuk posisi berbeda

---

## 5. Move Representation (`move.hpp`)

### 5.1 Move Construction Tests

- [x] `test_move_make` - Verifikasi normal move construction
- [x] `test_move_make_promotion` - Verifikasi promotion move (N/B/R/Q)
- [x] `test_move_make_enpassant` - Verifikasi en passant move
- [x] `test_move_make_castling` - Verifikasi castling move

### 5.2 Move Accessor Tests

- [x] `test_move_from` - Verifikasi extraction source square
- [x] `test_move_to` - Verifikasi extraction target square
- [x] `test_move_type` - Verifikasi move type detection
- [x] `test_move_promotion_type` - Verifikasi promotion piece type

### 5.3 Move Type Check Tests

- [x] `test_is_promotion` - Verifikasi promotion detection
- [x] `test_is_enpassant` - Verifikasi en passant detection
- [x] `test_is_castling` - Verifikasi castling detection
- [x] `test_is_normal` - Verifikasi normal move detection

### 5.4 Move String Conversion Tests

- [x] `test_move_to_string` - Verifikasi move -> UCI string
- [x] `test_move_to_string_promotion` - Verifikasi promotion notation
- [x] `test_string_to_move` - Verifikasi UCI string -> move
- [x] `test_string_to_move_promotion` - Verifikasi parsing promotions

### 5.5 MoveList Tests

- [x] `test_movelist_add` - Verifikasi adding moves
- [x] `test_movelist_size` - Verifikasi size tracking
- [x] `test_movelist_pick_best` - Verifikasi partial sorting

---

## 6. Board Representation (`board.hpp`, `board.cpp`)

### 6.1 Construction & FEN Tests

- [x] `test_board_default` - Default constructor = startpos
- [x] `test_board_fen_startpos` - Parse starting position FEN
- [x] `test_board_fen_complex` - Parse complex FEN positions
- [x] `test_board_fen_castling` - Parse semua castling variations
- [x] `test_board_fen_enpassant` - Parse en passant squares
- [x] `test_board_fen_generation` - Generate FEN dari board

### 6.2 Piece Access Tests

- [x] `test_piece_on` - Verifikasi piece lookup
- [x] `test_empty_square` - Verifikasi empty square detection
- [x] `test_pieces_by_type` - Verifikasi bitboard by piece type
- [x] `test_pieces_by_color` - Verifikasi bitboard by color
- [x] `test_king_square` - Verifikasi king position lookup
- [x] `test_piece_count` - Verifikasi piece counting

### 6.3 State Access Tests

- [x] `test_side_to_move` - Verifikasi current side
- [x] `test_castling_rights` - Verifikasi castling state
- [x] `test_en_passant_square` - Verifikasi ep square
- [x] `test_halfmove_clock` - Verifikasi 50-move counter
- [x] `test_game_ply` - Verifikasi ply counter

### 6.4 Attack Detection Tests

- [x] `test_attackers_to` - Verifikasi semua attackers ke square
- [x] `test_is_attacked_by` - Verifikasi attack by specific color
- [x] `test_slider_blockers` - Verifikasi pinned pieces
- [x] `test_in_check` - Verifikasi check detection
- [x] `test_checkers` - Verifikasi checker bitboard

### 6.5 Move Making Tests

- [x] `test_do_move_quiet` - Verifikasi quiet moves
- [x] `test_do_move_capture` - Verifikasi captures
- [x] `test_do_move_promotion` - Verifikasi promotions
- [x] `test_do_move_enpassant` - Verifikasi en passant
- [x] `test_do_move_castling` - Verifikasi castling
- [x] `test_undo_move` - Verifikasi move undo
- [x] `test_do_null_move` - Verifikasi null move
- [x] `test_undo_null_move` - Verifikasi null move undo

### 6.6 Key Computation Tests

- [x] `test_compute_key` - Verifikasi position key
- [x] `test_compute_pawn_key` - Verifikasi pawn key
- [x] `test_compute_material_key` - Verifikasi material key
- [x] `test_key_incremental` - Key update vs recomputation

### 6.7 Validation Tests

- [x] `test_is_valid` - Verifikasi position validation
- [x] `test_is_capture` - Verifikasi capture detection

---

## 7. Move Generation (`movegen.hpp`, `movegen.cpp`)

### 7.1 Pawn Move Tests

- [x] `test_pawn_single_push` - Single pawn pushes
- [x] `test_pawn_double_push` - Double pawn pushes dari rank 2/7
- [x] `test_pawn_captures` - Pawn captures
- [x] `test_pawn_promotion` - All promotion types
- [x] `test_pawn_enpassant` - En passant captures

### 7.2 Piece Move Tests

- [x] `test_knight_moves` - Knight move generation
- [x] `test_bishop_moves` - Bishop move generation
- [x] `test_rook_moves` - Rook move generation
- [x] `test_queen_moves` - Queen move generation
- [x] `test_king_moves` - King move generation

### 7.3 Castling Tests

- [x] `test_castling_kingside_white` - White O-O
- [x] `test_castling_queenside_white` - White O-O-O
- [x] `test_castling_kingside_black` - Black O-O
- [x] `test_castling_queenside_black` - Black O-O-O
- [x] `test_castling_blocked` - Castling dengan pieces blocking
- [x] `test_castling_through_check` - Castling melalui attack
- [x] `test_castling_out_of_check` - Castling dari check (illegal)

### 7.4 Move Generation Mode Tests

- [x] `test_generate_all` - All pseudo-legal moves
- [x] `test_generate_captures` - Capture moves only
- [x] `test_generate_quiets` - Quiet moves only
- [x] `test_generate_evasions` - Check evasions
- [x] `test_generate_legal` - All legal moves
- [x] `test_generate_checking_moves` - Moves that give check

### 7.5 Legality Tests

- [x] `test_is_legal` - Move legality validation
- [x] `test_is_pseudo_legal` - Pseudo-legality check
- [x] `test_gives_check` - Check prediction

### 7.6 Perft Tests

- [x] `test_perft_startpos_depth1` - Perft(1) = 20
- [x] `test_perft_startpos_depth2` - Perft(2) = 400
- [x] `test_perft_startpos_depth3` - Perft(3) = 8,902
- [x] `test_perft_startpos_depth4` - Perft(4) = 197,281
- [x] `test_perft_kiwipete` - Kiwipete position
- [x] `test_perft_position3` - Position 3
- [x] `test_perft_position4` - Position 4
- [x] `test_perft_position5` - Position 5
- [x] `test_perft_position6` - Position 6

---

## 8. Move Ordering (`moveorder.hpp`, `moveorder.cpp`)

### 8.1 SEE (Static Exchange Evaluation) Tests

- [ ] `test_see_equal_capture` - Equal exchange (e.g., NxN)
- [ ] `test_see_winning_capture` - Winning capture (PxQ)
- [ ] `test_see_losing_capture` - Losing capture (QxP defended)
- [ ] `test_see_complex` - Complex exchange sequences
- [ ] `test_see_ge` - SEE threshold comparison

### 8.2 MVV-LVA Tests

- [ ] `test_mvv_lva_ordering` - Verify capture ordering

### 8.3 Killer Moves Tests

- [ ] `test_killer_store` - Store killer moves
- [ ] `test_killer_is_killer` - Check if move is killer
- [ ] `test_killer_clear` - Clear killers

### 8.4 CounterMove Tests

- [ ] `test_countermove_store` - Store counter moves
- [ ] `test_countermove_get` - Retrieve counter moves

### 8.5 History Table Tests

- [ ] `test_history_update` - Update history scores
- [ ] `test_history_get` - Get history scores
- [ ] `test_history_clear` - Clear history

### 8.6 Continuation History Tests

- [ ] `test_cont_history_update` - Update continuation history
- [ ] `test_cont_history_get` - Get continuation history

### 8.7 Capture History Tests

- [ ] `test_capture_history_update` - Update capture history
- [ ] `test_capture_history_get` - Get capture scores

### 8.8 MovePicker Tests

- [ ] `test_movepicker_tt_first` - TT move returned first
- [ ] `test_movepicker_captures_before_quiets` - Good captures before quiets
- [ ] `test_movepicker_killer_ordering` - Killer move priority
- [ ] `test_movepicker_stages` - Verify all stages

---

## 9. Transposition Table (`tt.hpp`, `tt.cpp`)

### 9.1 TTEntry Tests

- [ ] `test_ttentry_save` - Save entry data
- [ ] `test_ttentry_move` - Retrieve move
- [ ] `test_ttentry_score` - Retrieve score
- [ ] `test_ttentry_depth` - Retrieve depth
- [ ] `test_ttentry_bound` - Retrieve bound type
- [ ] `test_ttentry_generation` - Retrieve generation

### 9.2 TranspositionTable Tests

- [ ] `test_tt_resize` - Resize table
- [ ] `test_tt_clear` - Clear table
- [ ] `test_tt_probe_miss` - Probe with no entry
- [ ] `test_tt_probe_hit` - Probe with existing entry
- [ ] `test_tt_replacement` - Entry replacement policy
- [ ] `test_tt_hashfull` - Calculate fill percentage
- [ ] `test_tt_prefetch` - Prefetch functionality

### 9.3 Score Adjustment Tests

- [ ] `test_score_to_tt` - Mate score adjustment
- [ ] `test_score_from_tt` - Mate score restoration

---

## 10. Evaluation (`eval.hpp`, `eval.cpp`)

### 10.1 Material Tests

- [ ] `test_eval_material_equal` - Equal material = ~0
- [ ] `test_eval_material_advantage` - Material advantage score
- [ ] `test_eval_material_pst` - PST contribution

### 10.2 Pawn Structure Tests

- [ ] `test_eval_doubled_pawns` - Doubled pawn penalty
- [ ] `test_eval_isolated_pawns` - Isolated pawn penalty
- [ ] `test_eval_backward_pawns` - Backward pawn penalty
- [ ] `test_eval_passed_pawns` - Passed pawn bonus
- [ ] `test_eval_connected_pawns` - Connected pawns bonus
- [ ] `test_eval_phalanx` - Phalanx bonus
- [ ] `test_eval_pawn_islands` - Pawn island count credit

### 10.3 Piece Evaluation Tests

- [ ] `test_eval_bishop_pair` - Bishop pair bonus
- [ ] `test_eval_rook_open_file` - Rook on open file
- [ ] `test_eval_rook_semiopen_file` - Rook on semi-open file
- [ ] `test_eval_rook_seventh_rank` - Rook on 7th rank
- [ ] `test_eval_knight_outpost` - Knight outpost bonus
- [ ] `test_eval_bishop_outpost` - Bishop outpost bonus
- [ ] `test_eval_bad_bishop` - Bad bishop penalty
- [ ] `test_eval_long_diagonal_bishop` - Long diagonal bonus
- [ ] `test_eval_trapped_pieces` - Trapped piece detection
- [ ] `test_eval_mobility` - Piece mobility

### 10.4 King Safety Tests

- [ ] `test_eval_king_pawn_shield` - Pawn shield evaluation
- [ ] `test_eval_king_open_files` - Open files near king penalty
- [ ] `test_eval_king_attackers` - Attacker count/weight
- [ ] `test_eval_king_zone_attacks` - King zone attack evaluation
- [ ] `test_eval_castled_king` - Castled king bonus

### 10.5 Threats & Space Tests

- [ ] `test_eval_threats` - Threat evaluation
- [ ] `test_eval_space` - Space control

### 10.6 Game Phase Tests

- [ ] `test_eval_phase_calculation` - Phase calculation
- [ ] `test_eval_tapered_eval` - Tapered evaluation

### 10.7 Special Position Tests

- [ ] `test_eval_symmetry` - Symmetric position = equal
- [ ] `test_eval_known_endgames` - Known endgame evaluations
- [ ] `test_eval_material_imbalance` - Material imbalance evaluation

---

## 11. Search (`search.hpp`, `search.cpp`)

### 11.1 Basic Search Tests

- [ ] `test_search_mate_in_1` - Find mate in 1
- [ ] `test_search_mate_in_2` - Find mate in 2
- [ ] `test_search_avoid_mate` - Avoid getting mated
- [ ] `test_search_best_move` - Find tactical best move

### 11.2 Alpha-Beta Tests

- [ ] `test_search_principal_variation` - PV extraction
- [ ] `test_search_aspiration_windows` - Aspiration window handling
- [ ] `test_search_fail_high` - Fail high behavior
- [ ] `test_search_fail_low` - Fail low behavior

### 11.3 Pruning Tests

- [ ] `test_null_move_pruning` - Null move pruning
- [ ] `test_futility_pruning` - Futility pruning
- [ ] `test_razoring` - Razoring
- [ ] `test_reverse_futility` - Reverse futility pruning
- [ ] `test_late_move_pruning` - LMP
- [ ] `test_see_pruning` - SEE-based pruning
- [ ] `test_history_pruning` - History-based pruning
- [ ] `test_probcut` - ProbCut

### 11.4 Reduction Tests

- [ ] `test_late_move_reduction` - LMR
- [ ] `test_iir` - Internal iterative reductions

### 11.5 Extension Tests

- [ ] `test_check_extension` - Check extension
- [ ] `test_singular_extension` - Singular extension
- [ ] `test_double_extension` - Double extension

### 11.6 Quiescence Search Tests

- [ ] `test_qsearch_captures` - Capture sequence
- [ ] `test_qsearch_delta_pruning` - Delta pruning
- [ ] `test_qsearch_stand_pat` - Stand pat

### 11.7 Time Management Tests

- [ ] `test_time_management_init` - Initialize time
- [ ] `test_time_management_check` - Time check
- [ ] `test_time_management_extension` - Time extension

---

## 12. Threading (`thread.hpp`, `thread.cpp`)

### 12.1 SearchThread Tests

- [ ] `test_thread_creation` - Create search thread
- [ ] `test_thread_start_searching` - Start search
- [ ] `test_thread_wait` - Wait for search completion
- [ ] `test_thread_clear_history` - Clear thread history

### 12.2 ThreadPool Tests

- [ ] `test_threadpool_set_count` - Set thread count
- [ ] `test_threadpool_start_thinking` - Start parallel search
- [ ] `test_threadpool_stop` - Stop all threads
- [ ] `test_threadpool_total_nodes` - Aggregate nodes
- [ ] `test_threadpool_best_move` - Get best move

### 12.3 Lazy SMP Tests

- [ ] `test_lazysmp_parallel_search` - Parallel search correctness
- [ ] `test_lazysmp_shared_tt` - Shared TT access

---

## 13. UCI Protocol (`uci.hpp`, `uci.cpp`)

### 13.1 Command Parsing Tests

- [ ] `test_uci_uci` - UCI command response
- [ ] `test_uci_isready` - isready/readyok
- [ ] `test_uci_ucinewgame` - New game
- [ ] `test_uci_position_startpos` - Position startpos
- [ ] `test_uci_position_fen` - Position FEN
- [ ] `test_uci_position_moves` - Position with moves
- [ ] `test_uci_go_depth` - Go depth
- [ ] `test_uci_go_movetime` - Go movetime
- [ ] `test_uci_go_wtime_btime` - Go with time control
- [ ] `test_uci_go_infinite` - Go infinite
- [ ] `test_uci_stop` - Stop command
- [ ] `test_uci_setoption` - Set option

### 13.2 UCI Options Tests

- [ ] `test_option_hash` - Hash size option
- [ ] `test_option_threads` - Thread count option
- [ ] `test_option_multipv` - MultiPV option
- [ ] `test_option_ponder` - Ponder option

---

## 14. Opening Book (`book.hpp`, `book.cpp`)

### 14.1 Polyglot Book Tests

- [ ] `test_book_load` - Load book file
- [ ] `test_book_probe` - Probe book move
- [ ] `test_book_polyglot_key` - Polyglot key calculation

---

## 15. Data Generation (`datagen.hpp`, `datagen.cpp`)

### 15.1 DataGenerator Tests

- [ ] `test_datagen_config` - Parse configuration
- [ ] `test_datagen_entry_encoding` - Entry encoding
- [ ] `test_datagen_entry_to_fen` - Entry to FEN

### 15.2 Binpack Tests

- [ ] `test_binpack_read` - Read binpack file
- [ ] `test_binpack_stats` - File statistics
- [ ] `test_binpack_filter` - Filter entries

---

## 16. Known Position Tests

### 16.1 Tactical Tests (WAC, etc.)

- [ ] `test_wac_positions` - Win at Chess positions
- [ ] `test_bratko_kopec` - Bratko-Kopec test suite

### 16.2 Benchmark Positions

- [ ] `test_benchmark_positions` - Standard benchmark positions

---

## Test Implementation Notes

### Recommended Test Framework

- **Catch2**

### Test Organization

```
tests/
├── test_types.cpp
├── test_bitboard.cpp
├── test_magic.cpp
├── test_zobrist.cpp
├── test_move.cpp
├── test_board.cpp
├── test_movegen.cpp
├── test_moveorder.cpp
├── test_tt.cpp
├── test_eval.cpp
├── test_search.cpp
├── test_thread.cpp
├── test_uci.cpp
├── test_book.cpp
├── test_datagen.cpp
└── test_tactical.cpp
```

### Running Tests

```bash
# Build tests
make tests

# Run all tests
./run_tests

# Run specific test file
./run_tests --gtest_filter="Bitboard*"
```

---

## Priority

1. **High Priority** (Core functionality)

   - Bitboard, Move, Board, MoveGen, Perft

2. **Medium Priority** (Search quality)

   - Evaluation, Search, TT, MoveOrder

3. **Lower Priority** (Supporting features)
   - UCI, Threading, Book, DataGen
