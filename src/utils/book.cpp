#include "book.hpp"

namespace Book {

OpeningBook book;

U64 PolyglotRandomPieceData[768];
U64 PolyglotRandomCastlingData[16];
U64 PolyglotRandomEnPassantData[8];
U64 PolyglotRandomTurnData;

const U64* PolyglotRandomPiece = PolyglotRandomPieceData;
const U64* PolyglotRandomCastling = PolyglotRandomCastlingData;
const U64* PolyglotRandomEnPassant = PolyglotRandomEnPassantData;
const U64 PolyglotRandomTurn = 0xF8D626AAAF278509ULL;

namespace {
    struct PolyglotInit {
        PolyglotInit() {
            std::mt19937_64 rng(0x1234567890ABCDEFULL);

            for (int i = 0; i < 768; ++i) {
                PolyglotRandomPieceData[i] = rng();
            }

            PolyglotRandomCastlingData[0] = 0ULL;
            for (int i = 1; i < 16; ++i) {
                PolyglotRandomCastlingData[i] = rng();
            }

            for (int i = 0; i < 8; ++i) {
                PolyglotRandomEnPassantData[i] = rng();
            }

            PolyglotRandomTurnData = rng();
        }
    } polyglot_init;
}

}
