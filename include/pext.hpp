#ifndef PEXT_HPP
#define PEXT_HPP

#include "types.hpp"

// ============================================================================
// PEXT/PDEP Bitboard Support (BMI2 Instructions)
//
// When USE_PEXT is defined, use PEXT for faster sliding piece attack lookups.
// PEXT extracts bits from a source based on a mask, providing O(1) perfect hashing.
// This eliminates the magic multiply and shift entirely.
//
// Requirements:
// - Intel Haswell (4th gen) or later
// - AMD Excavator or Zen 3+ (Zen 1/2 PEXT is slow due to microcode)
// - 64-bit compiler with BMI2 support
// ============================================================================

// Check if we can use hardware PEXT
#ifdef USE_PEXT

// Check for proper compiler and platform support
#if defined(_MSC_VER) && defined(_M_X64)
    // MSVC 64-bit
    #include <immintrin.h>
    #define HAS_PEXT_HARDWARE 1
#elif defined(__GNUC__) && defined(__x86_64__) && defined(__BMI2__)
    // GCC/Clang 64-bit with BMI2
    #include <x86intrin.h>
    #define HAS_PEXT_HARDWARE 1
#elif defined(__GNUC__) && defined(__x86_64__)
    // GCC/Clang 64-bit, try with x86intrin
    #include <x86intrin.h>
    // BMI2 might be enabled via -mbmi2 flag
    #if defined(__BMI2__)
        #define HAS_PEXT_HARDWARE 1
    #endif
#endif

#ifdef HAS_PEXT_HARDWARE

// Hardware PEXT/PDEP using BMI2 instructions
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

#endif // HAS_PEXT_HARDWARE

#endif // USE_PEXT

// ============================================================================
// PEXT-based Attack Lookup
//
// When USE_PEXT is enabled with hardware support, we use PEXT for attack lookup.
// This is faster on modern CPUs with BMI2 (Intel Haswell+, AMD Zen 3+).
// ============================================================================

#ifdef USE_PEXT

// PEXT attack lookup structure (simpler than magic)
struct PextEntry {
    Bitboard  mask;      // Relevant occupancy mask
    Bitboard* attacks;   // Attack table pointer

    // Direct PEXT-based index
    unsigned index(Bitboard occupied) const {
        return static_cast<unsigned>(pext(occupied, mask));
    }
};

#endif

#endif // PEXT_HPP
