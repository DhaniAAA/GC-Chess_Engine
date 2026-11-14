#include "tablebase.hpp"
#include "bitboard.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Tablebase {

Tablebases TB;

namespace EndgameRules {

namespace {

enum KpkResult : uint8_t {
    KPK_INVALID = 0,
    KPK_DRAW    = 1,
    KPK_WIN     = 2
};

// Normalized KPK table: [strong_to_move (0=weak, 1=strong)][pawn_idx 0..23][strong_king 0..63][weak_king 0..63]
uint8_t kpkTable[2][24][64][64];
bool kpkInitialized = false;

inline int chebyshev_dist(Square a, Square b) {
    return std::max(std::abs(int(file_of(a)) - int(file_of(b))),
                    std::abs(int(rank_of(a)) - int(rank_of(b))));
}

inline int kpk_pawn_index(Square sq) {
    return (int(rank_of(sq)) - 1) * 4 + int(file_of(sq));
}

inline Square kpk_pawn_square(int pidx) {
    return make_square(File(pidx % 4), Rank(1 + pidx / 4));
}

inline bool is_safe_promotion_win(Square wk, Square bk, Square promoSq) {
    if (promoSq == wk || promoSq == bk) return false;
    // If weak king can capture promoted queen immediately and strong king doesn't defend it -> draw
    if (chebyshev_dist(bk, promoSq) <= 1 && chebyshev_dist(wk, promoSq) > 1) {
        return false;
    }
    return true;
}

} // namespace

void init_kpk() {
    if (kpkInitialized) return;

    for (int stm = 0; stm < 2; ++stm) {
        for (int pidx = 0; pidx < 24; ++pidx) {
            Square psq = kpk_pawn_square(pidx);
            Bitboard pAtk = pawn_attacks_bb(WHITE, psq);
            for (int wk = 0; wk < 64; ++wk) {
                for (int bk = 0; bk < 64; ++bk) {
                    Square wSq = Square(wk);
                    Square bSq = Square(bk);
                    if (wSq == psq || bSq == psq || chebyshev_dist(wSq, bSq) <= 1) {
                        kpkTable[stm][pidx][wk][bk] = KPK_INVALID;
                    } else if (stm == 1 && (pAtk & square_bb(bSq))) {
                        kpkTable[stm][pidx][wk][bk] = KPK_INVALID;
                    } else {
                        kpkTable[stm][pidx][wk][bk] = KPK_DRAW;
                    }
                }
            }
        }
    }

    // Seed immediate safe 7th-rank promotion wins
    for (int pidx = 20; pidx < 24; ++pidx) { // RANK_7 -> pidx 20..23
        Square psq = kpk_pawn_square(pidx);
        Square promoSq = Square(int(psq) + 8);
        for (int wk = 0; wk < 64; ++wk) {
            for (int bk = 0; bk < 64; ++bk) {
                if (kpkTable[1][pidx][wk][bk] == KPK_INVALID) continue;
                if (is_safe_promotion_win(Square(wk), Square(bk), promoSq)) {
                    kpkTable[1][pidx][wk][bk] = KPK_WIN;
                }
            }
        }
    }

    // Retrograde fixed-point propagation
    bool changed = true;
    while (changed) {
        changed = false;
        for (int pidx = 23; pidx >= 0; --pidx) {
            Square psq = kpk_pawn_square(pidx);
            Bitboard pAtk = pawn_attacks_bb(WHITE, psq);

            for (int wk = 0; wk < 64; ++wk) {
                Square wSq = Square(wk);
                for (int bk = 0; bk < 64; ++bk) {
                    Square bSq = Square(bk);

                    // Strong side (WHITE) to move: WIN if ANY legal move reaches a WIN
                    if (kpkTable[1][pidx][wk][bk] == KPK_DRAW) {
                        bool wins = false;

                        // Pawn pushes (ranks 2..6 advance; rank 7 already seeded)
                        if (rank_of(psq) < RANK_7) {
                            Square push1 = Square(int(psq) + 8);
                            if (push1 != wSq && push1 != bSq) {
                                int np = kpk_pawn_index(push1);
                                if (kpkTable[0][np][wk][bk] == KPK_WIN) {
                                    wins = true;
                                }
                                if (!wins && rank_of(psq) == RANK_2) {
                                    Square push2 = Square(int(psq) + 16);
                                    if (push2 != wSq && push2 != bSq) {
                                        int np2 = kpk_pawn_index(push2);
                                        if (kpkTable[0][np2][wk][bk] == KPK_WIN) {
                                            wins = true;
                                        }
                                    }
                                }
                            }
                        }

                        // Strong King moves
                        if (!wins) {
                            Bitboard km = king_attacks_bb(wSq);
                            while (km) {
                                Square wk2 = pop_lsb(km);
                                if (wk2 != psq && chebyshev_dist(wk2, bSq) > 1) {
                                    if (kpkTable[0][pidx][wk2][bk] == KPK_WIN) {
                                        wins = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (wins) {
                            kpkTable[1][pidx][wk][bk] = KPK_WIN;
                            changed = true;
                        }
                    }

                    // Weak side (BLACK) to move: WIN if >=1 legal move AND ALL legal moves lose
                    if (kpkTable[0][pidx][wk][bk] == KPK_DRAW) {
                        bool hasLegal = false;
                        bool allLose = true;
                        Bitboard km = king_attacks_bb(bSq);
                        while (km) {
                            Square bk2 = pop_lsb(km);
                            if (chebyshev_dist(bk2, wSq) <= 1) continue;
                            if (pAtk & square_bb(bk2)) continue;
                            hasLegal = true;
                            if (bk2 == psq) {
                                // Weak king captures undefended pawn -> K vs K draw
                                allLose = false;
                                break;
                            }
                            if (kpkTable[1][pidx][wk][bk2] != KPK_WIN) {
                                allLose = false;
                                break;
                            }
                        }
                        if (hasLegal && allLose) {
                            kpkTable[0][pidx][wk][bk] = KPK_WIN;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    kpkInitialized = true;
}

static bool extract_normalized_kpk(const Board& board, Color& strongColor, int& pidx, Square& wk, Square& bk, bool& strongToMove) {
    if (popcount(board.pieces()) != 3 || popcount(board.pieces(PAWN)) != 1) {
        return false;
    }
    strongColor = board.pieces(WHITE, PAWN) ? WHITE : BLACK;
    Square psq = lsb(board.pieces(strongColor, PAWN));
    wk = board.king_square(strongColor);
    bk = board.king_square(~strongColor);
    strongToMove = (board.side_to_move() == strongColor);

    if (strongColor == BLACK) {
        psq = Square(int(psq) ^ 56);
        wk = Square(int(wk) ^ 56);
        bk = Square(int(bk) ^ 56);
    }
    if (file_of(psq) >= FILE_E) {
        psq = Square(int(psq) ^ 7);
        wk = Square(int(wk) ^ 7);
        bk = Square(int(bk) ^ 7);
    }
    pidx = kpk_pawn_index(psq);
    return (pidx >= 0 && pidx < 24);
}

bool probe_kpk(const Board& board) {
    if (!kpkInitialized) init_kpk();
    Color strongColor = WHITE;
    int pidx = 0;
    Square wk = SQ_NONE, bk = SQ_NONE;
    bool strongToMove = true;
    if (!extract_normalized_kpk(board, strongColor, pidx, wk, bk, strongToMove)) {
        return false;
    }
    return kpkTable[strongToMove ? 1 : 0][pidx][wk][bk] == KPK_WIN;
}

bool eval_kpk(const Board& board, int& outScore) {
    if (!kpkInitialized) init_kpk();
    Color strongColor = WHITE;
    int pidx = 0;
    Square wk = SQ_NONE, bk = SQ_NONE;
    bool strongToMove = true;
    if (!extract_normalized_kpk(board, strongColor, pidx, wk, bk, strongToMove)) {
        return false;
    }
    if (kpkTable[strongToMove ? 1 : 0][pidx][wk][bk] != KPK_WIN) {
        outScore = 0;
        return true;
    }
    Square psq = kpk_pawn_square(pidx);
    // Keep winning KPK score comfortably below promoted K+Q vs K (~1300+ cp) so search eagerly promotes.
    int winScore = 700 + int(rank_of(psq)) * 25 + (7 - chebyshev_dist(wk, psq)) * 8
                   + (7 - chebyshev_dist(wk, bk)) * 4;
    outScore = strongToMove ? winScore : -winScore;
    return true;
}

} // namespace EndgameRules

} // namespace Tablebase
