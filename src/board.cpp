#include "board.hpp"
#include "eval.hpp"
#include <sstream>
#include <iostream>
#include <cstring>

const std::string Board::StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Piece characters for FEN
const char PieceToChar[] = " PNBRQK  pnbrqk";

namespace Position {

void init() {
    Bitboards::init();
    Magics::init();
    Zobrist::init();
}

}

Board::Board() {
    clear();
    set(StartFEN, &startState);
}

Board::Board(const std::string& fen) {
    clear();
    set(fen, &startState);
}

void Board::clear() {
    std::memset(this, 0, sizeof(Board));

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        board[s] = NO_PIECE;
        castlingRightsMask[s] = ALL_CASTLING;
        pieceIndex[s] = 0;
    }

    for (int pc = 0; pc < PIECE_NB; ++pc) {
        for (int i = 0; i < MAX_PIECES_PER_TYPE; ++i) {
            pieceList[pc][i] = SQ_NONE;
        }
    }

    st = &startState;
    st->enPassant = SQ_NONE;
}

void Board::put_piece(Piece pc, Square s) {
    board[s] = pc;
    Bitboard bb = square_bb(s);
    byTypeBB[0] |= bb;
    byTypeBB[type_of(pc)] |= bb;
    byColorBB[color_of(pc)] |= bb;

    pieceIndex[s] = pieceCount[pc];
    pieceList[pc][pieceCount[pc]] = s;
    pieceCount[pc]++;
    pieceCount[make_piece(color_of(pc), NO_PIECE_TYPE)]++;

    if (type_of(pc) == KING) {
        kingSquare[color_of(pc)] = s;
    }
}

void Board::remove_piece(Square s) {
    Piece pc = board[s];
    Bitboard bb = square_bb(s);
    byTypeBB[0] ^= bb;
    byTypeBB[type_of(pc)] ^= bb;
    byColorBB[color_of(pc)] ^= bb;
    board[s] = NO_PIECE;

    int idx = pieceIndex[s];
    pieceCount[pc]--;
    Square lastSq = pieceList[pc][pieceCount[pc]];
    pieceList[pc][idx] = lastSq;
    pieceIndex[lastSq] = idx;
    pieceList[pc][pieceCount[pc]] = SQ_NONE;

    pieceCount[make_piece(color_of(pc), NO_PIECE_TYPE)]--;
}

void Board::move_piece(Square from, Square to) {
    Piece pc = board[from];
    Bitboard fromTo = square_bb(from) | square_bb(to);

    byTypeBB[0] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;

    board[from] = NO_PIECE;
    board[to] = pc;

    int idx = pieceIndex[from];
    pieceList[pc][idx] = to;
    pieceIndex[to] = idx;

    if (type_of(pc) == KING) {
        kingSquare[color_of(pc)] = to;
    }
}

void Board::set(const std::string& fen, StateInfo* si) {
    clear();
    st = si;
    std::memset(st, 0, sizeof(StateInfo));
    st->enPassant = SQ_NONE;

    std::istringstream ss(fen);
    std::string token;

    ss >> token;
    Square sq = SQ_A8;

    for (char c : token) {
        if (c == '/') {
            sq = Square(sq - 16);
        } else if (c >= '1' && c <= '8') {
            sq = Square(sq + (c - '0'));
        } else {
            size_t idx = std::string(PieceToChar).find(c);
            if (idx != std::string::npos) {
                put_piece(Piece(idx), sq);
                ++sq;
            }
        }
    }

    ss >> token;
    sideToMove = (token == "w") ? WHITE : BLACK;

    ss >> token;
    st->castling = NO_CASTLING;

    for (char c : token) {
        switch (c) {
            case 'K': st->castling |= WHITE_OO; break;
            case 'Q': st->castling |= WHITE_OOO; break;
            case 'k': st->castling |= BLACK_OO; break;
            case 'q': st->castling |= BLACK_OOO; break;
            default: break;
        }
    }

    castlingRightsMask[SQ_A1] = CastlingRights(ALL_CASTLING ^ WHITE_OOO);
    castlingRightsMask[SQ_E1] = CastlingRights(ALL_CASTLING ^ WHITE_CASTLING);
    castlingRightsMask[SQ_H1] = CastlingRights(ALL_CASTLING ^ WHITE_OO);
    castlingRightsMask[SQ_A8] = CastlingRights(ALL_CASTLING ^ BLACK_OOO);
    castlingRightsMask[SQ_E8] = CastlingRights(ALL_CASTLING ^ BLACK_CASTLING);
    castlingRightsMask[SQ_H8] = CastlingRights(ALL_CASTLING ^ BLACK_OO);

    ss >> token;
    if (token != "-" && token.length() == 2) {
        st->enPassant = string_to_square(token);
    }

    if (ss >> token) {
        st->halfmoveClock = std::stoi(token);
    }
    if (ss >> token) {
        gamePly = (std::stoi(token) - 1) * 2 + (sideToMove == BLACK);
    }

    set_state(si);
}

void Board::set_state(StateInfo* si) {
    si->positionKey = compute_key();
    si->pawnKey = compute_pawn_key();
    si->materialKey = compute_material_key();

    si->psqtScore[WHITE] = EvalScore(0, 0);
    si->psqtScore[BLACK] = EvalScore(0, 0);

    Bitboard bb = pieces();
    while (bb) {
        Square sq = pop_lsb(bb);
        Piece pc = piece_on(sq);
        si->psqtScore[color_of(pc)] += Eval::piece_pst_score(pc, sq);
    }

    set_check_info();
}

std::string Board::fen() const {
    std::ostringstream ss;

    for (Rank r = RANK_8; r >= RANK_1; --r) {
        int emptyCount = 0;

        for (File f = FILE_A; f <= FILE_H; ++f) {
            Square s = make_square(f, r);
            Piece pc = piece_on(s);

            if (pc == NO_PIECE) {
                emptyCount++;
            } else {
                if (emptyCount > 0) {
                    ss << emptyCount;
                    emptyCount = 0;
                }
                ss << PieceToChar[pc];
            }
        }

        if (emptyCount > 0) {
            ss << emptyCount;
        }

        if (r > RANK_1) {
            ss << '/';
        }
    }

    ss << (sideToMove == WHITE ? " w " : " b ");

    if (st->castling == NO_CASTLING) {
        ss << '-';
    } else {
        if (st->castling & WHITE_OO)  ss << 'K';
        if (st->castling & WHITE_OOO) ss << 'Q';
        if (st->castling & BLACK_OO)  ss << 'k';
        if (st->castling & BLACK_OOO) ss << 'q';
    }

    ss << ' ';
    if (st->enPassant == SQ_NONE) {
        ss << '-';
    } else {
        ss << square_to_string(st->enPassant);
    }

    ss << ' ' << st->halfmoveClock;

    ss << ' ' << (gamePly / 2 + 1);

    return ss.str();
}

Key Board::compute_key() const {
    Key k = 0;

    Bitboard bb = pieces();
    while (bb) {
        Square s = pop_lsb(bb);
        k ^= Zobrist::piece_key(piece_on(s), s);
    }

    k ^= Zobrist::castling_key(st->castling);

    if (st->enPassant != SQ_NONE) {
        k ^= Zobrist::enpassant_key(file_of(st->enPassant));
    }

    if (sideToMove == BLACK) {
        k ^= Zobrist::side_key();
    }

    return k;
}

Key Board::compute_pawn_key() const {
    Key k = 0;

    Bitboard pawns = pieces(PAWN);
    while (pawns) {
        Square s = pop_lsb(pawns);
        k ^= Zobrist::piece_key(piece_on(s), s);
    }

    return k;
}

Key Board::compute_material_key() const {
    Key k = 0;

    for (Color c : {WHITE, BLACK}) {
        for (PieceType pt = PAWN; pt <= KING; ++pt) {
            Piece pc = make_piece(c, pt);
            for (int i = 0; i < pieceCount[pc]; ++i) {
                k ^= Zobrist::piece_key(pc, Square(i));  // Use index as pseudo-square
            }
        }
    }

    return k;
}

Bitboard Board::attackers_to(Square s) const {
    return attackers_to(s, pieces());
}

Bitboard Board::attackers_to(Square s, Bitboard occupied) const {
    return (pawn_attacks_bb(BLACK, s) & pieces(WHITE, PAWN))
         | (pawn_attacks_bb(WHITE, s) & pieces(BLACK, PAWN))
         | (knight_attacks_bb(s) & pieces(KNIGHT))
         | (rook_attacks_bb(s, occupied) & pieces(ROOK, QUEEN))
         | (bishop_attacks_bb(s, occupied) & pieces(BISHOP, QUEEN))
         | (king_attacks_bb(s) & pieces(KING));
}

bool Board::is_attacked_by(Color c, Square s) const {
    return attackers_to(s) & pieces(c);
}

Bitboard Board::slider_blockers(Bitboard sliders, Square s, Bitboard& pinners_out) const {
    Bitboard blockers = EMPTY_BB;
    pinners_out = EMPTY_BB;

    Bitboard snipers = ((rook_attacks_bb(s, EMPTY_BB) & pieces(ROOK, QUEEN)) |
                        (bishop_attacks_bb(s, EMPTY_BB) & pieces(BISHOP, QUEEN))) & sliders;

    Bitboard occupancy = pieces() ^ snipers;

    while (snipers) {
        Square sniper_sq = pop_lsb(snipers);
        Bitboard between = between_bb(s, sniper_sq) & occupancy;

        if (between && !more_than_one(between)) {
            blockers |= between;

            if (between & pieces(color_of(piece_on(s)))) {
                pinners_out |= sniper_sq;
            }
        }
    }

    return blockers;
}

void Board::set_check_info() {
    st->checkers = attackers_to(king_square(sideToMove)) & pieces(~sideToMove);

    Color us = sideToMove;
    Color them = ~us;

    st->blockersForKing[us] = slider_blockers(pieces(them), king_square(us), st->pinners[them]);
    st->blockersForKing[them] = slider_blockers(pieces(us), king_square(them), st->pinners[us]);

    Square ksq = king_square(them);

    st->checkSquares[PAWN] = pawn_attacks_bb(them, ksq);
    st->checkSquares[KNIGHT] = knight_attacks_bb(ksq);
    st->checkSquares[BISHOP] = bishop_attacks_bb(ksq, pieces());
    st->checkSquares[ROOK] = rook_attacks_bb(ksq, pieces());
    st->checkSquares[QUEEN] = st->checkSquares[BISHOP] | st->checkSquares[ROOK];
    st->checkSquares[KING] = EMPTY_BB;
}

bool Board::is_valid() const {
    if (popcount(pieces(WHITE, KING)) != 1) return false;
    if (popcount(pieces(BLACK, KING)) != 1) return false;

    if (attackers_to(king_square(~sideToMove)) & pieces(sideToMove)) return false;

    if (pieces(PAWN) & (RANK_1_BB | RANK_8_BB)) return false;

    if (st->enPassant != SQ_NONE) {
        Rank ep_rank = (sideToMove == WHITE) ? RANK_6 : RANK_3;
        if (rank_of(st->enPassant) != ep_rank) return false;
    }

    return true;
}

std::string Board::pretty() const {
    std::ostringstream ss;

    ss << "\n +---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_8; r >= RANK_1; --r) {
        for (File f = FILE_A; f <= FILE_H; ++f) {
            Square s = make_square(f, r);
            Piece pc = piece_on(s);

            ss << " | " << PieceToChar[pc];
        }
        ss << " | " << (r + 1) << "\n";
        ss << " +---+---+---+---+---+---+---+---+\n";
    }

    ss << "   a   b   c   d   e   f   g   h\n\n";
    ss << "FEN: " << fen() << "\n";
    ss << "Key: " << std::hex << key() << std::dec << "\n";
    ss << (sideToMove == WHITE ? "White" : "Black") << " to move\n";

    if (in_check()) {
        ss << "IN CHECK!\n";
    }

    return ss.str();
}

void Board::do_move(Move m, StateInfo& newSt) {
    Color us = sideToMove;
    Color them = ~us;
    Square from = m.from();
    Square to = m.to();
    Piece pc = piece_on(from);
    Piece captured = m.is_enpassant() ? make_piece(them, PAWN) : piece_on(to);

    std::memcpy(&newSt, st, sizeof(StateInfo));
    newSt.previous = st;
    st = &newSt;

    Key k = st->positionKey ^ Zobrist::side_key();

    if (captured != NO_PIECE) {
        Square capsq = to;

        if (m.is_enpassant()) {
            capsq = to - pawn_push(us);
        }

        st->psqtScore[them] -= Eval::piece_pst_score(captured, capsq);

        remove_piece(capsq);

        k ^= Zobrist::piece_key(captured, capsq);

        if (type_of(captured) == PAWN) {
            st->pawnKey ^= Zobrist::piece_key(captured, capsq);
        }

        st->halfmoveClock = 0;
    } else {
        st->halfmoveClock++;
    }

    st->capturedPiece = captured;

    CastlingRights oldCastling = st->castling;
    st->castling &= castlingRightsMask[from] & castlingRightsMask[to];
    k ^= Zobrist::castling_key(oldCastling) ^ Zobrist::castling_key(st->castling);

    if (st->enPassant != SQ_NONE) {
        k ^= Zobrist::enpassant_key(file_of(st->enPassant));
        st->enPassant = SQ_NONE;
    }
    if (m.is_castling()) {
        Square rfrom, rto;

        if (to > from) {
            rfrom = Square(from + 3);
            rto = Square(from + 1);
        } else {
            rfrom = Square(from - 4);
            rto = Square(from - 1);
        }

        st->psqtScore[us] -= Eval::piece_pst_score(pc, from);
        st->psqtScore[us] += Eval::piece_pst_score(pc, to);

        k ^= Zobrist::piece_key(pc, from) ^ Zobrist::piece_key(pc, to);
        move_piece(from, to);

        Piece rook = piece_on(rfrom);
        st->psqtScore[us] -= Eval::piece_pst_score(rook, rfrom);
        st->psqtScore[us] += Eval::piece_pst_score(rook, rto);
        k ^= Zobrist::piece_key(rook, rfrom) ^ Zobrist::piece_key(rook, rto);
        move_piece(rfrom, rto);

    } else if (m.is_promotion()) {
        Piece promoted = make_piece(us, m.promotion_type());

        st->psqtScore[us] -= Eval::piece_pst_score(pc, from);
        st->psqtScore[us] += Eval::piece_pst_score(promoted, to);

        k ^= Zobrist::piece_key(pc, from);
        remove_piece(from);

        k ^= Zobrist::piece_key(promoted, to);
        put_piece(promoted, to);

        st->pawnKey ^= Zobrist::piece_key(pc, from);

        st->halfmoveClock = 0;

    } else {
        st->psqtScore[us] -= Eval::piece_pst_score(pc, from);
        st->psqtScore[us] += Eval::piece_pst_score(pc, to);

        k ^= Zobrist::piece_key(pc, from) ^ Zobrist::piece_key(pc, to);
        move_piece(from, to);

        if (type_of(pc) == PAWN) {
            st->halfmoveClock = 0;
            st->pawnKey ^= Zobrist::piece_key(pc, from) ^ Zobrist::piece_key(pc, to);
            if (std::abs(int(to) - int(from)) == 16) {
                Square ep_sq = Square((from + to) / 2);

                if (pawn_attacks_bb(us, ep_sq) & pieces(them, PAWN)) {
                    st->enPassant = ep_sq;
                    k ^= Zobrist::enpassant_key(file_of(ep_sq));
                }
            }
        }
    }

    st->positionKey = k;
    st->pliesFromNull++;

    st->repetition = 0;
    int end = std::min(st->halfmoveClock, st->pliesFromNull);
    if (end >= 4) {
        StateInfo* stp = st->previous->previous;
        for (int i = 4; i <= end; i += 2) {
            stp = stp->previous->previous;
            if (stp->positionKey == st->positionKey) {
                st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }

    sideToMove = them;
    ++gamePly;

    set_check_info();
}

void Board::undo_move(Move m) {
    Color them = sideToMove;
    Color us = ~them;
    Square from = m.from();
    Square to = m.to();
    Piece pc = piece_on(to);

    if (m.is_castling()) {
        Square rfrom, rto;

        if (to > from) {
            rfrom = Square(from + 3);
            rto = Square(from + 1);
        } else {
            rfrom = Square(from - 4);
            rto = Square(from - 1);
        }

        move_piece(rto, rfrom);
        move_piece(to, from);

    } else if (m.is_promotion()) {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), from);

    } else {
        move_piece(to, from);
    }

    Piece captured = st->capturedPiece;
    if (captured != NO_PIECE) {
        Square capsq = to;

        if (m.is_enpassant()) {
            capsq = to - pawn_push(us);
        }

        put_piece(captured, capsq);
    }

    st = st->previous;
    sideToMove = us;
    --gamePly;
}

void Board::do_null_move(StateInfo& newSt) {
    std::memcpy(&newSt, st, sizeof(StateInfo));
    newSt.previous = st;
    st = &newSt;
    if (st->enPassant != SQ_NONE) {
        st->positionKey ^= Zobrist::enpassant_key(file_of(st->enPassant));
        st->enPassant = SQ_NONE;
    }

    st->positionKey ^= Zobrist::side_key();
    st->pliesFromNull = 0;
    st->halfmoveClock++;

    sideToMove = ~sideToMove;
    ++gamePly;

    set_check_info();
}

void Board::undo_null_move() {
    st = st->previous;
    sideToMove = ~sideToMove;
    --gamePly;
}

bool Board::is_draw(int ply) const {
    if (st->halfmoveClock >= 100) {
        if (in_check()) {
            return true;
        }
        return true;
    }

    if (st->repetition) {
        if (st->repetition < 0) {
            return true;
        }

        if (ply >= st->repetition) {
            return true;
        }
    }

    int totalPieces = popcount(pieces());

    if (totalPieces == 2) {
        return true;
    }

    if (totalPieces == 3) {
        int knights = popcount(pieces(KNIGHT));
        int bishops = popcount(pieces(BISHOP));
        if (knights + bishops == 1) {
            return true;
        }
    }

    if (totalPieces == 4) {
        if (popcount(pieces(KNIGHT)) == 2 && popcount(pieces(BISHOP)) == 0) {
            return true;
        }

        if (popcount(pieces(BISHOP)) == 2 && popcount(pieces(KNIGHT)) == 0) {
            Bitboard whiteBishops = pieces(WHITE, BISHOP);
            Bitboard blackBishops = pieces(BLACK, BISHOP);
            if (popcount(whiteBishops) == 1 && popcount(blackBishops) == 1) {
                Square wbSq = lsb(whiteBishops);
                Square bbSq = lsb(blackBishops);
                bool wbLight = ((file_of(wbSq) + rank_of(wbSq)) % 2) == 1;
                bool bbLight = ((file_of(bbSq) + rank_of(bbSq)) % 2) == 1;
                if (wbLight == bbLight) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool Board::has_repeated() const {
    return st->repetition != 0;
}
