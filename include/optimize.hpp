#ifndef OPTIMIZE_HPP
#define OPTIMIZE_HPP

#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)

    #define FORCE_INLINE __attribute__((always_inline)) inline
    #define NEVER_INLINE __attribute__((noinline))

    #define HOT_FUNCTION __attribute__((hot))
    #define COLD_FUNCTION __attribute__((cold))

    #define PREFETCH_READ(addr)      __builtin_prefetch((addr), 0, 3)
    #define PREFETCH_WRITE(addr)     __builtin_prefetch((addr), 1, 3)
    #define PREFETCH_READ_LOW(addr)  __builtin_prefetch((addr), 0, 0)
    #define PREFETCH_READ_T1(addr)   __builtin_prefetch((addr), 0, 2)
    #define PREFETCH_READ_T2(addr)   __builtin_prefetch((addr), 0, 1)

    #define ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)

#elif defined(_MSC_VER)
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)

    #define FORCE_INLINE __forceinline
    #define NEVER_INLINE __declspec(noinline)

    #define HOT_FUNCTION
    #define COLD_FUNCTION

    #include <intrin.h>
    #define PREFETCH_READ(addr)      _mm_prefetch((const char*)(addr), _MM_HINT_T0)
    #define PREFETCH_WRITE(addr)     _mm_prefetch((const char*)(addr), _MM_HINT_T0)
    #define PREFETCH_READ_LOW(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_NTA)
    #define PREFETCH_READ_T1(addr)   _mm_prefetch((const char*)(addr), _MM_HINT_T1)
    #define PREFETCH_READ_T2(addr)   _mm_prefetch((const char*)(addr), _MM_HINT_T2)

    #define ASSUME(cond) __assume(cond)

#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)

    #define FORCE_INLINE inline
    #define NEVER_INLINE

    #define HOT_FUNCTION
    #define COLD_FUNCTION

    #define PREFETCH_READ(addr)
    #define PREFETCH_WRITE(addr)
    #define PREFETCH_READ_LOW(addr)
    #define PREFETCH_READ_T1(addr)
    #define PREFETCH_READ_T2(addr)

    #define ASSUME(cond) ((void)0)
#endif

constexpr size_t CACHE_LINE_SIZE_OPT = 64;

constexpr int PREFETCH_STRIDE = 4;

#define PREFETCH_NEXT(arr, idx, stride) \
    PREFETCH_READ(&(arr)[(idx) + (stride)])

#define PREFETCH_TT_ENTRY(addr) PREFETCH_READ(addr)
#define PREFETCH_HISTORY(addr) PREFETCH_READ_T1(addr)
#define PREFETCH_MOVELIST(addr) PREFETCH_READ(addr)

#if defined(__GNUC__) || defined(__clang__)
    #define LOOP_EXPECT_ITERATIONS(n)

    #if defined(__clang__)
        #define LOOP_UNROLL(n) _Pragma("clang loop unroll_count(" #n ")")
    #else
        #define LOOP_UNROLL(n) _Pragma("GCC unroll " #n)
    #endif
#else
    #define LOOP_EXPECT_ITERATIONS(n)
    #define LOOP_UNROLL(n)
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")
#elif defined(_MSC_VER)
    #define COMPILER_BARRIER() _ReadWriteBarrier()
#else
    #define COMPILER_BARRIER()
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define RESTRICT __restrict__
#elif defined(_MSC_VER)
    #define RESTRICT __restrict
#else
    #define RESTRICT
#endif

#define CACHE_ALIGNED alignas(64)
#define PAD_TO_CACHE_LINE(bytes_used) \
    char _padding[CACHE_LINE_SIZE_OPT - ((bytes_used) % CACHE_LINE_SIZE_OPT)]

constexpr size_t padding_for_cache_line(size_t size) {
    return (CACHE_LINE_SIZE_OPT - (size % CACHE_LINE_SIZE_OPT)) % CACHE_LINE_SIZE_OPT;
}

#if defined(__GNUC__) || defined(__clang__)
    #define ASSUME_ALIGNED(ptr, align) __builtin_assume_aligned((ptr), (align))
#else
    #define ASSUME_ALIGNED(ptr, align) (ptr)
#endif

#define HOT_DATA_BEGIN
#define HOT_DATA_END

template<typename T>
inline void prefetch_next_iteration(const T* RESTRICT arr, int current_idx, int max_idx, int distance = 2) {
    int next_idx = current_idx + distance;
    if (next_idx < max_idx) {
        PREFETCH_READ(&arr[next_idx]);
    }
}

#define PREFETCH_BOARD_STATE(board_ptr) \
    PREFETCH_READ((board_ptr))
template<typename T>
inline void prefetch_batch(const T* arr, int start, int count) {
    for (int i = 0; i < count && i < 4; ++i) {
        PREFETCH_READ(&arr[start + i]);
    }
}

#endif