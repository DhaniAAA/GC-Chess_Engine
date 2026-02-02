#include "movegen.hpp"
#include "profiler.hpp"
#include "optimize.hpp"

void MoveGen::add_promotions(MoveList& moves, Square from, Square to) {
    moves.add(Move::make_promotion(from, to, QUEEN));
    moves.add(Move::make_promotion(from, to, ROOK));
    moves.add(Move::make_promotion(from, to, BISHOP));
    moves.add(Move::make_promotion(from, to, KNIGHT));
}

template<Color Us>
void MoveGen::generate_pawn_moves(const Board& board, MoveList& moves, Bitboard target) {
    constexpr Color Them = (Us == WHITE) ? BLACK : WHITE;
    constexpr Direction Up = (Us == WHITE) ? NORTH : SOUTH;
    constexpr Direction UpLeft = (Us == WHITE) ? NORTH_WEST : SOUTH_WEST;
    constexpr Direction UpRight = (Us == WHITE) ? NORTH_EAST : SOUTH_EAST;
    constexpr Bitboard Rank3BB = (Us == WHITE) ? RANK_3_BB : RANK_6_BB;
    constexpr Bitboard Rank7BB = (Us == WHITE) ? RANK_7_BB : RANK_2_BB;

    Bitboard pawns = board.pieces(Us, PAWN);
    Bitboard empty = ~board.pieces();
    Bitboard enemies = board.pieces(Them) & target;

    Bitboard pawns_on_7 = pawns & Rank7BB;
    Bitboard pawns_not_7 = pawns & ~Rank7BB;

    Bitboard push1 = shift<Up>(pawns_not_7) & empty;
    Bitboard push2 = shift<Up>(push1 & Rank3BB) & empty;

    push1 &= target;
    push2 &= target;

    while (push1) {
        Square to = pop_lsb(push1);
        Square from = to - Up;
        moves.add(Move::make(from, to));
    }

    while (push2) {
        Square to = pop_lsb(push2);
        Square from = to - Up - Up;
        moves.add(Move::make(from, to));
    }

    if (pawns_on_7) {
        Bitboard promo_push = shift<Up>(pawns_on_7) & empty & target;

        while (promo_push) {
            Square to = pop_lsb(promo_push);
            Square from = to - Up;
            add_promotions(moves, from, to);
        }
    }

    Bitboard cap_left = shift<UpLeft>(pawns_not_7) & enemies;
    Bitboard cap_right = shift<UpRight>(pawns_not_7) & enemies;

    while (cap_left) {
        Square to = pop_lsb(cap_left);
        Square from = to - UpLeft;
        moves.add(Move::make(from, to));
    }

    while (cap_right) {
        Square to = pop_lsb(cap_right);
        Square from = to - UpRight;
        moves.add(Move::make(from, to));
    }

    if (pawns_on_7) {
        Bitboard promo_cap_left = shift<UpLeft>(pawns_on_7) & enemies;
        Bitboard promo_cap_right = shift<UpRight>(pawns_on_7) & enemies;

        while (promo_cap_left) {
            Square to = pop_lsb(promo_cap_left);
            Square from = to - UpLeft;
            add_promotions(moves, from, to);
        }

        while (promo_cap_right) {
            Square to = pop_lsb(promo_cap_right);
            Square from = to - UpRight;
            add_promotions(moves, from, to);
        }
    }

    Square ep = board.en_passant_square();
    if (ep != SQ_NONE) {
        Square captured_pawn_sq = (Us == WHITE) ? Square(ep - 8) : Square(ep + 8);

        if ((target & ep) || (target & captured_pawn_sq)) {
            Bitboard ep_pawns = pawn_attacks_bb(Them, ep) & pawns;

            while (ep_pawns) {
                Square from = pop_lsb(ep_pawns);
                moves.add(Move::make_enpassant(from, ep));
            }
        }
    }
}

template<PieceType Pt>
void MoveGen::generate_piece_moves(const Board& board, MoveList& moves,
                                   Color us, Bitboard target) {
    Bitboard pieces = board.pieces(us, Pt);
    Bitboard occupied = board.pieces();

    while (pieces) {
        Square from = pop_lsb(pieces);
        Bitboard attacks;

        if constexpr (Pt == KNIGHT) {
            attacks = knight_attacks_bb(from);
        } else if constexpr (Pt == BISHOP) {
            attacks = bishop_attacks_bb(from, occupied);
        } else if constexpr (Pt == ROOK) {
            attacks = rook_attacks_bb(from, occupied);
        } else if constexpr (Pt == QUEEN) {
            attacks = queen_attacks_bb(from, occupied);
        } else if constexpr (Pt == KING) {
            attacks = king_attacks_bb(from);
        }

        attacks &= target;

        while (attacks) {
            Square to = pop_lsb(attacks);
            moves.add(Move::make(from, to));
        }
    }
}

template<Color Us>
void MoveGen::generate_castling(const Board& board, MoveList& moves) {
    constexpr CastlingRights KingSide = (Us == WHITE) ? WHITE_OO : BLACK_OO;
    constexpr CastlingRights QueenSide = (Us == WHITE) ? WHITE_OOO : BLACK_OOO;
    constexpr Square KingFrom = (Us == WHITE) ? SQ_E1 : SQ_E8;
    constexpr Square KingToOO = (Us == WHITE) ? SQ_G1 : SQ_G8;
    constexpr Square KingToOOO = (Us == WHITE) ? SQ_C1 : SQ_C8;

    CastlingRights cr = board.castling_rights();
    Bitboard occupied = board.pieces();
    Color them = ~Us;

    if (cr & KingSide) {
        constexpr Bitboard PathOO = (Us == WHITE) ?
            (square_bb(SQ_F1) | square_bb(SQ_G1)) :
            (square_bb(SQ_F8) | square_bb(SQ_G8));

        if (!(occupied & PathOO)) {
            constexpr Square F_sq = (Us == WHITE) ? SQ_F1 : SQ_F8;
            if (!board.is_attacked_by(them, KingFrom) &&
                !board.is_attacked_by(them, F_sq) &&
                !board.is_attacked_by(them, KingToOO)) {
                moves.add(Move::make_castling(KingFrom, KingToOO));
            }
        }
    }

    if (cr & QueenSide) {
        constexpr Bitboard PathOOO = (Us == WHITE) ?
            (square_bb(SQ_D1) | square_bb(SQ_C1) | square_bb(SQ_B1)) :
            (square_bb(SQ_D8) | square_bb(SQ_C8) | square_bb(SQ_B8));
        constexpr Bitboard CheckPath = (Us == WHITE) ?
            (square_bb(SQ_D1) | square_bb(SQ_C1)) :
            (square_bb(SQ_D8) | square_bb(SQ_C8));

        if (!(occupied & PathOOO)) {
            constexpr Square D_sq = (Us == WHITE) ? SQ_D1 : SQ_D8;
            if (!board.is_attacked_by(them, KingFrom) &&
                !board.is_attacked_by(them, D_sq) &&
                !board.is_attacked_by(them, KingToOOO)) {
                moves.add(Move::make_castling(KingFrom, KingToOOO));
            }
        }
    }
}

void MoveGen::generate_all(const Board& board, MoveList& moves) {
    PROFILE_SCOPE("generate_all");
    Color us = board.side_to_move();
    Bitboard target = ~board.pieces(us);

    if (us == WHITE) {
        generate_pawn_moves<WHITE>(board, moves, FULL_BB);
        generate_castling<WHITE>(board, moves);
    } else {
        generate_pawn_moves<BLACK>(board, moves, FULL_BB);
        generate_castling<BLACK>(board, moves);
    }

    generate_piece_moves<KNIGHT>(board, moves, us, target);
    generate_piece_moves<BISHOP>(board, moves, us, target);
    generate_piece_moves<ROOK>(board, moves, us, target);
    generate_piece_moves<QUEEN>(board, moves, us, target);
    generate_piece_moves<KING>(board, moves, us, target);
}

void MoveGen::generate_captures(const Board& board, MoveList& moves) {
    PROFILE_SCOPE("generate_captures");
    Color us = board.side_to_move();
    Color them = ~us;
    Bitboard target = board.pieces(them);

    if (us == WHITE) {
        generate_pawn_moves<WHITE>(board, moves, target);
    } else {
        generate_pawn_moves<BLACK>(board, moves, target);
    }

    generate_piece_moves<KNIGHT>(board, moves, us, target);
    generate_piece_moves<BISHOP>(board, moves, us, target);
    generate_piece_moves<ROOK>(board, moves, us, target);
    generate_piece_moves<QUEEN>(board, moves, us, target);
    generate_piece_moves<KING>(board, moves, us, target);
}

void MoveGen::generate_quiets(const Board& board, MoveList& moves) {
    PROFILE_SCOPE("generate_quiets");
    Color us = board.side_to_move();
    Bitboard target = ~board.pieces();

    if (us == WHITE) {
        generate_pawn_moves<WHITE>(board, moves, target);
        generate_castling<WHITE>(board, moves);
    } else {
        generate_pawn_moves<BLACK>(board, moves, target);
        generate_castling<BLACK>(board, moves);
    }

    generate_piece_moves<KNIGHT>(board, moves, us, target);
    generate_piece_moves<BISHOP>(board, moves, us, target);
    generate_piece_moves<ROOK>(board, moves, us, target);
    generate_piece_moves<QUEEN>(board, moves, us, target);
    generate_piece_moves<KING>(board, moves, us, target);
}

void MoveGen::generate_checking_moves(const Board& board, MoveList& moves) {
    Color us = board.side_to_move();
    Color them = ~us;
    Square theirKing = board.king_square(them);
    Bitboard occupied = board.pieces();
    Bitboard empty = ~occupied;
    Bitboard ourPieces = board.pieces(us);

    Bitboard knightCheckSqs = board.check_squares(KNIGHT);
    Bitboard bishopCheckSqs = board.check_squares(BISHOP);
    Bitboard rookCheckSqs = board.check_squares(ROOK);
    Bitboard queenCheckSqs = board.check_squares(QUEEN);

    Bitboard discoveredBlockers = board.blockers_for_king(them) & ourPieces;

    Bitboard knights = board.pieces(us, KNIGHT);
    while (knights) {
        Square from = pop_lsb(knights);
        Bitboard attacks = knight_attacks_bb(from);

        Bitboard directChecks = attacks & empty & knightCheckSqs;
        while (directChecks) {
            Square to = pop_lsb(directChecks);
            moves.add(Move::make(from, to));
        }

        if (discoveredBlockers & from) {
            Bitboard discMoves = attacks & empty & ~knightCheckSqs;
            while (discMoves) {
                Square to = pop_lsb(discMoves);
                if (!aligned(from, to, theirKing)) {
                    moves.add(Move::make(from, to));
                }
            }
        }
    }

    Bitboard bishops = board.pieces(us, BISHOP);
    while (bishops) {
        Square from = pop_lsb(bishops);
        Bitboard attacks = bishop_attacks_bb(from, occupied);

        Bitboard directChecks = attacks & empty & bishopCheckSqs;
        while (directChecks) {
            Square to = pop_lsb(directChecks);
            moves.add(Move::make(from, to));
        }

        if (discoveredBlockers & from) {
            Bitboard discMoves = attacks & empty & ~bishopCheckSqs;
            while (discMoves) {
                Square to = pop_lsb(discMoves);
                if (!aligned(from, to, theirKing)) {
                    moves.add(Move::make(from, to));
                }
            }
        }
    }

    Bitboard rooks = board.pieces(us, ROOK);
    while (rooks) {
        Square from = pop_lsb(rooks);
        Bitboard attacks = rook_attacks_bb(from, occupied);

        Bitboard directChecks = attacks & empty & rookCheckSqs;
        while (directChecks) {
            Square to = pop_lsb(directChecks);
            moves.add(Move::make(from, to));
        }

        if (discoveredBlockers & from) {
            Bitboard discMoves = attacks & empty & ~rookCheckSqs;
            while (discMoves) {
                Square to = pop_lsb(discMoves);
                if (!aligned(from, to, theirKing)) {
                    moves.add(Move::make(from, to));
                }
            }
        }
    }

    Bitboard queens = board.pieces(us, QUEEN);
    while (queens) {
        Square from = pop_lsb(queens);
        Bitboard attacks = queen_attacks_bb(from, occupied);

        Bitboard directChecks = attacks & empty & queenCheckSqs;
        while (directChecks) {
            Square to = pop_lsb(directChecks);
            moves.add(Move::make(from, to));
        }
    }

    Bitboard pawns = board.pieces(us, PAWN);
    Bitboard pawnCheckSqs = pawn_attacks_bb(them, theirKing);

    Bitboard rank7 = (us == WHITE) ? RANK_7_BB : RANK_2_BB;
    pawns &= ~rank7;

    Direction push = pawn_push(us);
    Bitboard rank3 = (us == WHITE) ? RANK_3_BB : RANK_6_BB;

    while (pawns) {
        Square from = pop_lsb(pawns);

        Square to1 = Square(from + push);
        if (board.empty(to1)) {
            if (pawnCheckSqs & to1) {
                moves.add(Move::make(from, to1));
            }
            else if ((discoveredBlockers & from) && !aligned(from, to1, theirKing)) {
                moves.add(Move::make(from, to1));
            }

            Rank fromRank = rank_of(from);
            if ((us == WHITE && fromRank == RANK_2) || (us == BLACK && fromRank == RANK_7)) {
                Square to2 = Square(to1 + push);
                if (board.empty(to2)) {
                    if (pawnCheckSqs & to2) {
                        moves.add(Move::make(from, to2));
                    }
                    else if ((discoveredBlockers & from) && !aligned(from, to2, theirKing)) {
                        moves.add(Move::make(from, to2));
                    }
                }
            }
        }
    }

    if (discoveredBlockers & board.king_square(us)) {
        Square ksq = board.king_square(us);
        Bitboard kingMoves = king_attacks_bb(ksq) & empty;

        while (kingMoves) {
            Square to = pop_lsb(kingMoves);
            if (!aligned(ksq, to, theirKing)) {
                Bitboard occ = (occupied ^ ksq) | to;
                if (!(board.attackers_to(to, occ) & board.pieces(them))) {
                    moves.add(Move::make(ksq, to));
                }
            }
        }
    }
}

void MoveGen::generate_evasions(const Board& board, MoveList& moves) {
    Color us = board.side_to_move();
    Color them = ~us;
    Square ksq = board.king_square(us);
    Bitboard checkers = board.checkers();

    Bitboard king_moves = king_attacks_bb(ksq) & ~board.pieces(us);
    Bitboard occupied = board.pieces() ^ square_bb(ksq);

    while (king_moves) {
        Square to = pop_lsb(king_moves);
        if (!(board.attackers_to(to, occupied) & board.pieces(them))) {
            moves.add(Move::make(ksq, to));
        }
    }

    if (more_than_one(checkers)) {
        return;
    }

    Square checker_sq = lsb(checkers);
    Bitboard target = between_bb(ksq, checker_sq) | checkers;

    if (us == WHITE) {
        generate_pawn_moves<WHITE>(board, moves, target);
    } else {
        generate_pawn_moves<BLACK>(board, moves, target);
    }

    generate_piece_moves<KNIGHT>(board, moves, us, target);
    generate_piece_moves<BISHOP>(board, moves, us, target);
    generate_piece_moves<ROOK>(board, moves, us, target);
    generate_piece_moves<QUEEN>(board, moves, us, target);
}

bool MoveGen::is_legal(const Board& board, Move m) {
    if (m.is_none()) return false;

    Color us = board.side_to_move();
    Square from = m.from();
    Square to = m.to();

    Piece pc = board.piece_on(from);
    if (pc == NO_PIECE || color_of(pc) != us) {
        return false;
    }

    if (board.pieces(us) & to) {
        return false;
    }

    Color them = ~us;
    Square ksq = board.king_square(us);

    if (m.is_enpassant()) {
        Square captured_sq = to - pawn_push(us);
        Bitboard occupied = (board.pieces() ^ from ^ captured_sq) | to;

        return !(rook_attacks_bb(ksq, occupied) & board.pieces(them, ROOK, QUEEN)) &&
               !(bishop_attacks_bb(ksq, occupied) & board.pieces(them, BISHOP, QUEEN));
    }

    if (type_of(board.piece_on(from)) == KING) {
        if (m.is_castling()) {
            if (board.in_check()) return false;

            if (us == WHITE) {
                if (to == SQ_G1) {
                    if (!(board.castling_rights() & WHITE_OO)) return false;
                    if (board.pieces() & (square_bb(SQ_F1) | square_bb(SQ_G1))) return false;
                    if (board.is_attacked_by(them, SQ_F1) || board.is_attacked_by(them, SQ_G1)) return false;
                } else if (to == SQ_C1) {
                    if (!(board.castling_rights() & WHITE_OOO)) return false;
                    if (board.pieces() & (square_bb(SQ_D1) | square_bb(SQ_C1) | square_bb(SQ_B1))) return false;
                    if (board.is_attacked_by(them, SQ_D1) || board.is_attacked_by(them, SQ_C1)) return false;
                } else return false;
            } else {
                if (to == SQ_G8) {
                    if (!(board.castling_rights() & BLACK_OO)) return false;
                    if (board.pieces() & (square_bb(SQ_F8) | square_bb(SQ_G8))) return false;
                    if (board.is_attacked_by(them, SQ_F8) || board.is_attacked_by(them, SQ_G8)) return false;
                } else if (to == SQ_C8) {
                    if (!(board.castling_rights() & BLACK_OOO)) return false;
                    if (board.pieces() & (square_bb(SQ_D8) | square_bb(SQ_C8) | square_bb(SQ_B8))) return false;
                    if (board.is_attacked_by(them, SQ_D8) || board.is_attacked_by(them, SQ_C8)) return false;
                } else return false;
            }
            return true;
        }
        Bitboard occupied = board.pieces() ^ from;
        return !(board.attackers_to(to, occupied) & board.pieces(them));
    }

    if (board.in_check()) {
        Bitboard checkers = board.checkers();

        if (more_than_one(checkers)) {
            return false;
        }

        Square checker_sq = lsb(checkers);
        Bitboard target = between_bb(ksq, checker_sq) | checkers;

        if (!(target & to)) {
            return false;
        }
    }

    if (board.blockers_for_king(us) & from) {
        return aligned(from, to, ksq);
    }

    return true;
}

void MoveGen::generate_legal(const Board& board, MoveList& moves) {
    PROFILE_SCOPE("generate_legal");
    if (board.in_check()) {
        generate_evasions(board, moves);
    } else {
        generate_all(board, moves);
    }

    int legal_count = 0;
    for (int i = 0; i < moves.size(); ++i) {
        if (is_legal(board, moves[i].move)) {
            moves[legal_count++] = moves[i];
        }
    }

    moves.resize(legal_count);
}

bool MoveGen::gives_check(const Board& board, Move m) {
    Square from = m.from();
    Square to = m.to();
    Color us = board.side_to_move();
    Square their_king = board.king_square(~us);

    PieceType pt = type_of(board.piece_on(from));
    if (m.is_promotion()) {
        pt = m.promotion_type();
    }

    if (board.check_squares(pt) & to) {
        return true;
    }

    if ((board.blockers_for_king(~us) & from) && !aligned(from, to, their_king)) {
        return true;
    }

    if (m.is_enpassant()) {
        Square captured = to - pawn_push(us);
        Bitboard occupied = (board.pieces() ^ from ^ captured) | to;

        return (rook_attacks_bb(their_king, occupied) & board.pieces(us, ROOK, QUEEN)) ||
               (bishop_attacks_bb(their_king, occupied) & board.pieces(us, BISHOP, QUEEN));
    }

    if (m.is_castling()) {
        Square rook_to = (to > from) ? Square(to - 1) : Square(to + 1);
        Bitboard occupied = (board.pieces() ^ from) | to | rook_to;
        return rook_attacks_bb(their_king, occupied) & rook_to;
    }

    return false;
}

bool MoveGen::is_pseudo_legal(const Board& board, Move m) {
    if (m.is_none()) return false;

    Color us = board.side_to_move();
    Square from = m.from();
    Square to = m.to();
    Piece pc = board.piece_on(from);

    if (pc == NO_PIECE || color_of(pc) != us) {
        return false;
    }

    if (board.pieces(us) & to) {
        return false;
    }

    PieceType pt = type_of(pc);

    if (pt == PAWN) {
        Direction push = pawn_push(us);
        Rank to_rank = rank_of(to);

        if (m.is_promotion()) {
            if (us == WHITE && to_rank != RANK_8) return false;
            if (us == BLACK && to_rank != RANK_1) return false;
        }

        if (m.is_enpassant()) {
            return to == board.en_passant_square() && (pawn_attacks_bb(us, from) & to);
        }

        if (to == from + push && board.empty(to)) {
            return true;
        }

        Rank from_rank = (us == WHITE) ? RANK_2 : RANK_7;
        if (rank_of(from) == from_rank && to == from + push + push &&
            board.empty(Square(from + push)) && board.empty(to)) {
            return true;
        }

        if ((pawn_attacks_bb(us, from) & to) && (board.pieces(~us) & to)) {
            return true;
        }

        return false;
    }

    if (m.is_castling()) {
        if (pt != KING) return false;
        if (board.in_check()) return false;

        if (us == WHITE) {
            if (to == SQ_G1) return (board.castling_rights() & WHITE_OO) && !(board.pieces() & (square_bb(SQ_F1) | square_bb(SQ_G1)));
            if (to == SQ_C1) return (board.castling_rights() & WHITE_OOO) && !(board.pieces() & (square_bb(SQ_D1) | square_bb(SQ_C1) | square_bb(SQ_B1)));
        } else {
            if (to == SQ_G8) return (board.castling_rights() & BLACK_OO) && !(board.pieces() & (square_bb(SQ_F8) | square_bb(SQ_G8)));
            if (to == SQ_C8) return (board.castling_rights() & BLACK_OOO) && !(board.pieces() & (square_bb(SQ_D8) | square_bb(SQ_C8) | square_bb(SQ_B8)));
        }
        return false;
    }

    Bitboard attacks = attacks_bb(pt, from, board.pieces());
    return attacks & to;
}
