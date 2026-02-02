#ifndef TABLEBASE_HPP
#define TABLEBASE_HPP

#include "board.hpp"
#include "move.hpp"
#include "tt.hpp"
#include <string>

namespace Tablebase {

enum WDLScore {
    WDL_LOSS         = -2,
    WDL_BLESSED_LOSS = -1,
    WDL_DRAW         =  0,
    WDL_CURSED_WIN   =  1,
    WDL_WIN          =  2,
    WDL_NONE         = -3
};

struct TablebaseConfig {
    std::string path;
    int maxPieces = 6;
    bool probeAtRoot = true;
    bool probeInSearch = true;
};

class Tablebases {
public:
    Tablebases() : initialized(false), maxPieces(0) {}

    bool init(const std::string& path) {
        tbPath = path;

        if (!path.empty()) {
            initialized = true;
            maxPieces = 6;
            return true;
        }

        return false;
    }

    bool is_initialized() const { return initialized; }

    int max_pieces() const { return maxPieces; }

    bool can_probe(const Board& board) const {
        if (!initialized) return false;

        int pieceCount = popcount(board.pieces());
        if (pieceCount > maxPieces) return false;

        if (board.castling_rights() != NO_CASTLING) return false;

        return true;
    }

    WDLScore probe_wdl(const Board& board) const {
        if (!can_probe(board)) return WDL_NONE;
        (void)board;
        return WDL_NONE;
    }

    int probe_dtz(const Board& board, Move& bestMove) const {
        if (!can_probe(board)) {
            bestMove = MOVE_NONE;
            return 0;
        }

        (void)board;
        bestMove = MOVE_NONE;
        return 0;
    }
    Move probe_root(const Board& board) const {
        Move bestMove = MOVE_NONE;
        probe_dtz(board, bestMove);
        return bestMove;
    }

    static int wdl_to_score(WDLScore wdl, int ply) {
        switch (wdl) {
            case WDL_WIN:         return VALUE_MATE - ply - 100;
            case WDL_CURSED_WIN:  return 1;
            case WDL_DRAW:        return 0;
            case WDL_BLESSED_LOSS: return -1;
            case WDL_LOSS:        return -(VALUE_MATE - ply - 100);
            default:              return 0;
        }
    }

private:
    bool initialized;
    int maxPieces;
    std::string tbPath;
};

extern Tablebases TB;

namespace EndgameRules {
inline bool is_known_draw(const Board& board) {
    int whitePawns = popcount(board.pieces(WHITE, PAWN));
    int blackPawns = popcount(board.pieces(BLACK, PAWN));
    int whiteKnights = popcount(board.pieces(WHITE, KNIGHT));
    int blackKnights = popcount(board.pieces(BLACK, KNIGHT));
    int whiteBishops = popcount(board.pieces(WHITE, BISHOP));
    int blackBishops = popcount(board.pieces(BLACK, BISHOP));
    int whiteRooks = popcount(board.pieces(WHITE, ROOK));
    int blackRooks = popcount(board.pieces(BLACK, ROOK));
    int whiteQueens = popcount(board.pieces(WHITE, QUEEN));
    int blackQueens = popcount(board.pieces(BLACK, QUEEN));

    int whitePieces = whiteKnights + whiteBishops + whiteRooks + whiteQueens;
    int blackPieces = blackKnights + blackBishops + blackRooks + blackQueens;

    if (whitePieces == 0 && blackPieces == 0 && whitePawns == 0 && blackPawns == 0) {
        return true;
    }

    if (whitePawns == 0 && blackPawns == 0) {
        if (whitePieces == 0 && blackPieces == 1 && blackKnights == 1) return true;
        if (blackPieces == 0 && whitePieces == 1 && whiteKnights == 1) return true;
    }

    if (whitePawns == 0 && blackPawns == 0) {
        if (whitePieces == 0 && blackPieces == 1 && blackBishops == 1) return true;
        if (blackPieces == 0 && whitePieces == 1 && whiteBishops == 1) return true;
    }
    if (whitePawns == 0 && blackPawns == 0) {
        if (whitePieces == 0 && blackPieces == 2 && blackKnights == 2) return true;
        if (blackPieces == 0 && whitePieces == 2 && whiteKnights == 2) return true;
    }

    if (whitePawns == 0 && blackPawns == 0 &&
        whitePieces == 1 && blackPieces == 1 &&
        whiteBishops == 1 && blackBishops == 1) {
        Square wb = lsb(board.pieces(WHITE, BISHOP));
        Square bb = lsb(board.pieces(BLACK, BISHOP));
        bool whiteOnLight = ((file_of(wb) + rank_of(wb)) % 2) == 0;
        bool blackOnLight = ((file_of(bb) + rank_of(bb)) % 2) == 0;
        if (whiteOnLight != blackOnLight) {
            return true;
        }
    }

    return false;
}

inline int scale_factor(const Board& board) {
    int whitePawns = popcount(board.pieces(WHITE, PAWN));
    int blackPawns = popcount(board.pieces(BLACK, PAWN));

    if (whitePawns == 0 && blackPawns == 0) {
        return 32;
    }

    if (whitePawns == 0 || blackPawns == 0) {
        return 64;
    }

    return 128;
}

}

}

#endif
