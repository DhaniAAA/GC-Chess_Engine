#ifndef PEXT_HPP
#define PEXT_HPP

#include "types.hpp"

#ifdef USE_PEXT

#if defined(_MSC_VER) && defined(_M_X64)
    #include <immintrin.h>
    #define HAS_PEXT_HARDWARE 1
#elif defined(__GNUC__) && defined(__x86_64__) && defined(__BMI2__)
    #include <x86intrin.h>
    #define HAS_PEXT_HARDWARE 1
#elif defined(__GNUC__) && defined(__x86_64__)
    #include <x86intrin.h>
    #if defined(__BMI2__)
        #define HAS_PEXT_HARDWARE 1
    #endif
#endif

#ifdef HAS_PEXT_HARDWARE

inline U64 pext(U64 src, U64 mask) {
    return _pext_u64(src, mask);
}

inline U64 pdep(U64 src, U64 mask) {
    return _pdep_u64(src, mask);
}

#else


inline U64 pext(U64 src, U64 mask) {
    U64 result = 0;
    int count = 0;
    while (mask) {
        if (src & (mask & -mask)) {
            result |= (1ULL << count);
        }
        mask &= mask - 1;
        ++count;
    }
    return result;
}

inline U64 pdep(U64 src, U64 mask) {
    U64 result = 0;
    int count = 0;
    while (mask) {
        if (src & (1ULL << count)) {
            result |= (mask & -mask);
        }
        mask &= mask - 1;
        ++count;
    }
    return result;
}

#endif

#endif

#ifdef USE_PEXT

struct PextEntry {
    Bitboard  mask;
    Bitboard* attacks;

    unsigned index(Bitboard occupied) const {
        return static_cast<unsigned>(pext(occupied, mask));
    }
};

#endif

#endif
