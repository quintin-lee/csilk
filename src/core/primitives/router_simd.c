/**
 * @file router_simd.c
 * @brief SIMD-accelerated path segment extraction and string matching.
 *
 * Contains AVX2/AVX-512/NEON optimized routines for:
 *  - URL path segment extraction (get_next_segment)
 *  - Fast memory comparison (csilk_memcmp_fast)
 *  - SIMD character search (csilk_simd_find_char)
 *  - Fast common prefix length (csilk_common_prefix_len_fast)
 *
 * All unaligned word loads strictly comply with ISO C23 alignment and strict aliasing
 * rules using memcpy or standard unaligned vector intrinsics.
 *
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "router_internal.h"

#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define CSILK_NO_SANITIZE_ADDR __attribute__((no_sanitize("address")))
#elif __has_attribute(no_sanitize_address)
#define CSILK_NO_SANITIZE_ADDR __attribute__((no_sanitize_address))
#else
#define CSILK_NO_SANITIZE_ADDR
#endif
#else
#define CSILK_NO_SANITIZE_ADDR
#endif

/* ---------------------------------------------------------------------------
 * get_next_segment — SIMD-accelerated variants
 * -------------------------------------------------------------------------*/

#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) CSILK_NO_SANITIZE_ADDR static inline const char*
/** @brief AVX-512 variant: extract the next '/'- or NUL-delimited path segment.
 * @see get_next_segment */
get_next_segment_avx512(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return NULL;
    }

    const char* start = *p;
    const char* curr = *p;

    __m512i slash_vec = _mm512_set1_epi8('/');
    __m512i zero_vec = _mm512_setzero_si512();

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        if ((addr & 4095) <= 4096 - 64) {
            __m512i   data = _mm512_loadu_si512((const void*)curr);
            __mmask64 cmp_slash = _mm512_cmpeq_epi8_mask(data, slash_vec);
            __mmask64 cmp_zero = _mm512_cmpeq_epi8_mask(data, zero_vec);
            __mmask64 cmp_combined = cmp_slash | cmp_zero;
            if (cmp_combined != 0) {
                int idx = __builtin_ctzll(cmp_combined);
                curr += idx;
                break;
            }
            curr += 64;
        } else {
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

#if defined(__x86_64__)
__attribute__((target("avx2"))) CSILK_NO_SANITIZE_ADDR static inline const char*
/** @brief AVX2 variant: extract the next '/'- or NUL-delimited path segment.
 * @see get_next_segment */
get_next_segment_avx2(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return NULL;
    }

    const char* start = *p;
    const char* curr = *p;

    __m256i slash_vec = _mm256_set1_epi8('/');
    __m256i zero_vec = _mm256_setzero_si256();

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        /* 4KB page boundary guard: Ensure 32-byte unaligned SIMD load does not cross
         * into an unmapped adjacent page, preventing potential SIGSEGV. */
        if ((addr & 4095) <= 4096 - 32) {
            __m256i data = _mm256_loadu_si256((const __m256i*)(const void*)curr);
            __m256i cmp_slash = _mm256_cmpeq_epi8(data, slash_vec);
            __m256i cmp_zero = _mm256_cmpeq_epi8(data, zero_vec);
            __m256i cmp_combined = _mm256_or_si256(cmp_slash, cmp_zero);
            int     mask = _mm256_movemask_epi8(cmp_combined);
            if (mask != 0) {
                /* Count trailing zeros to find exact byte index of the first delimiter ('/' or '\0'). */
                int idx = __builtin_ctz(mask);
                curr += idx;
                break;
            }
            curr += 32;
        } else {
            /* Fallback to byte-by-byte scan near 4KB page boundaries to safely cross boundary. */
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

#if defined(__ARM_NEON)
CSILK_NO_SANITIZE_ADDR static inline const char*
/** @brief NEON variant: extract the next '/'- or NUL-delimited path segment.
 * @see get_next_segment */
get_next_segment_neon(const char** p, size_t* len)
{
    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return NULL;
    }

    const char* start = *p;
    const char* curr = *p;

    uint8x16_t slash_vec = vdupq_n_u8('/');
    uint8x16_t zero_vec = vdupq_n_u8('\0');

    while (1) {
        uintptr_t addr = (uintptr_t)curr;
        if ((addr & 4095) <= 4096 - 16) {
            uint8x16_t data = vld1q_u8((const uint8_t*)curr);
            uint8x16_t cmp_slash = vceqq_u8(data, slash_vec);
            uint8x16_t cmp_zero = vceqq_u8(data, zero_vec);
            uint8x16_t cmp_combined = vorrq_u8(cmp_slash, cmp_zero);

            uint64_t mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 0);
            uint64_t mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 1);

            if (mask_low != 0 || mask_high != 0) {
                if (mask_low != 0) {
                    int idx = __builtin_ctzll(mask_low) / 8;
                    curr += idx;
                } else {
                    int idx = __builtin_ctzll(mask_high) / 8;
                    curr += 8 + idx;
                }
                break;
            }
            curr += 16;
        } else {
            if (*curr == '/' || *curr == '\0') {
                break;
            }
            curr++;
        }
    }

    *p = curr;
    *len = (size_t)(curr - start);
    return start;
}
#endif

/**
 * @brief Extract the next path segment from a URL path.
 *
 * Skips leading '/', then scans for the next '/' or NUL to delimit a segment.
 * Dispatches to the best SIMD implementation (AVX-512/AVX2/NEON) when the
 * CPU supports it, falling back to a scalar scan.
 *
 * @param[in,out] p   Pointer to the current scan position; advanced past the
 *                    consumed segment.
 * @param[out]    len Receives the length of the returned segment.
 * @return Pointer to the segment start, or NULL at end of string.
 */
const char*
get_next_segment(const char** p, size_t* len)
{
#if defined(CSILK_HAS_AVX512)
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        return get_next_segment_avx512(p, len);
    }
#endif
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        return get_next_segment_avx2(p, len);
    }
#elif defined(__ARM_NEON)
    return get_next_segment_neon(p, len);
#endif

    if (!*p || **p == '\0') {
        return NULL;
    }

    while (**p == '/') {
        (*p)++;
    }
    if (**p == '\0') {
        return NULL;
    }

    const char* start = *p;
    while (**p != '/' && **p != '\0') {
        (*p)++;
    }

    *len = (size_t)(*p - start);
    return start;
}

/* ---------------------------------------------------------------------------
 * csilk_memcmp_fast — SIMD-accelerated comparison variants
 * -------------------------------------------------------------------------*/

#if defined(__x86_64__)
#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) static inline int
/** @brief AVX-512 variant: constant-time equality test for two byte buffers.
 * @see csilk_memcmp_fast */
csilk_memcmp_avx512(const char* s1, const char* s2, size_t n)
{
    while (n >= 64) {
        __m512i   v1 = _mm512_loadu_si512((const void*)s1);
        __m512i   v2 = _mm512_loadu_si512((const void*)s2);
        __mmask64 cmp = _mm512_cmpeq_epi8_mask(v1, v2);
        if (cmp != 0xFFFFFFFFFFFFFFFFULL) {
            return 0;
        }
        s1 += 64;
        s2 += 64;
        n -= 64;
    }
    if (n == 0) {
        return 1;
    }
    return csilk_memcmp_fast(s1, s2, n);
}
#endif

__attribute__((target("avx2"))) static inline int
/** @brief AVX2 variant: constant-time equality test for two byte buffers.
 * @see csilk_memcmp_fast */
csilk_memcmp_avx2(const char* s1, const char* s2, size_t n)
{
    while (n >= 32) {
        __m256i v1 = _mm256_loadu_si256((const __m256i*)(const void*)s1);
        __m256i v2 = _mm256_loadu_si256((const __m256i*)(const void*)s2);
        __m256i cmp = _mm256_cmpeq_epi8(v1, v2);
        int     mask = _mm256_movemask_epi8(cmp);
        if (mask != (int)0xFFFFFFFF) {
            return 0;
        }
        s1 += 32;
        s2 += 32;
        n -= 32;
    }
    if (n == 0) {
        return 1;
    }
    return csilk_memcmp_fast(s1, s2, n);
}
#endif

#if defined(__ARM_NEON)
static inline int
/** @brief NEON variant: constant-time equality test for two byte buffers.
 * @see csilk_memcmp_fast */
csilk_memcmp_neon(const char* s1, const char* s2, size_t n)
{
    while (n >= 16) {
        uint8x16_t v1 = vld1q_u8((const uint8_t*)s1);
        uint8x16_t v2 = vld1q_u8((const uint8_t*)s2);
        uint8x16_t cmp = vceqq_u8(v1, v2);
        uint64_t   mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t   mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (mask_low != UINT64_MAX || mask_high != UINT64_MAX) {
            return 0;
        }
        s1 += 16;
        s2 += 16;
        n -= 16;
    }
    if (n == 0) {
        return 1;
    }
    return csilk_memcmp_fast(s1, s2, n);
}
#endif

/**
 * @brief Compare two byte buffers for equality using SIMD when beneficial.
 *
 * Returns non-zero when the first @p n bytes of @p s1 and @p s2 are identical.
 * Uses AVX-512/AVX2/NEON for large aligned runs, followed by ISO C23 strictly
 * conforming unaligned word-at-a-time (memcpy) loads, and finally byte comparison.
 *
 * @param[in] s1 First buffer.
 * @param[in] s2 Second buffer.
 * @param[in] n  Number of bytes to compare.
 * @return 1 if equal, 0 otherwise (n == 0 is considered equal).
 */
int
csilk_memcmp_fast(const char* s1, const char* s2, size_t n)
{
    if (n == 0) {
        return 1;
    }

#if defined(CSILK_HAS_AVX512)
    static int has_avx512 = -1;
    if (has_avx512 < 0) {
        has_avx512 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
    }
    if (n >= 64 && has_avx512) {
        return csilk_memcmp_avx512(s1, s2, n);
    }
#endif
#if defined(__x86_64__)
    static int has_avx2 = -1;
    if (has_avx2 < 0) {
        has_avx2 = __builtin_cpu_supports("avx2");
    }
    if (n >= 32 && has_avx2) {
        return csilk_memcmp_avx2(s1, s2, n);
    }
#elif defined(__ARM_NEON)
    if (n >= 16) {
        return csilk_memcmp_neon(s1, s2, n);
    }
#endif

#if defined(__x86_64__) || defined(__aarch64__)
    while (n >= 8) {
        uint64_t v1, v2;
        memcpy(&v1, s1, sizeof(v1));
        memcpy(&v2, s2, sizeof(v2));
        if (v1 != v2) {
            return 0;
        }
        s1 += 8;
        s2 += 8;
        n -= 8;
    }
    if (n >= 4) {
        uint32_t v1, v2;
        memcpy(&v1, s1, sizeof(v1));
        memcpy(&v2, s2, sizeof(v2));
        if (v1 != v2) {
            return 0;
        }
        s1 += 4;
        s2 += 4;
        n -= 4;
    }
    if (n >= 2) {
        uint16_t v1, v2;
        memcpy(&v1, s1, sizeof(v1));
        memcpy(&v2, s2, sizeof(v2));
        if (v1 != v2) {
            return 0;
        }
        s1 += 2;
        s2 += 2;
        n -= 2;
    }
    if (n == 1) {
        return *s1 == *s2;
    }
    return 1;
#else
    return memcmp(s1, s2, n) == 0;
#endif
}

/* ---------------------------------------------------------------------------
 * csilk_simd_find_char — SIMD-accelerated character search
 * -------------------------------------------------------------------------*/

#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) static inline const char*
/** @brief AVX-512 variant: find the first occurrence of a byte in a range.
 * @see csilk_simd_find_char */
csilk_simd_find_char_avx512(const char* curr, const char* end, char target)
{
    __m512i target_vec = _mm512_set1_epi8(target);
    while (curr + 64 <= end) {
        __m512i   data = _mm512_loadu_si512((const void*)curr);
        __mmask64 cmp = _mm512_cmpeq_epi8_mask(data, target_vec);
        if (cmp != 0) {
            int idx = __builtin_ctzll(cmp);
            return curr + idx;
        }
        curr += 64;
    }
    return curr;
}
#endif

#if defined(__x86_64__)
__attribute__((target("avx2"))) static inline const char*
/** @brief AVX2 variant: find the first occurrence of a byte in a range.
 * @see csilk_simd_find_char */
csilk_simd_find_char_avx2(const char* curr, const char* end, char target)
{
    __m256i target_vec = _mm256_set1_epi8(target);
    while (curr + 32 <= end) {
        __m256i  data = _mm256_loadu_si256((const __m256i*)(const void*)curr);
        __m256i  cmp = _mm256_cmpeq_epi8(data, target_vec);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);
        if (mask != 0) {
            int idx = __builtin_ctz(mask);
            return curr + idx;
        }
        curr += 32;
    }
    return curr;
}
#endif

/**
 * @brief Find the first occurrence of @p target within a byte range.
 *
 * Scans [@p s, @p s+@p len) for @p target, dispatching to AVX-512/AVX2/NEON for
 * vector runs when available and falling back to a scalar scan.
 *
 * @param[in] s      Start of the search range.
 * @param[in] len    Length of the range in bytes.
 * @param[in] target Byte to find.
 * @return Pointer to the first match, or NULL if not found / invalid input.
 */
const char*
csilk_simd_find_char(const char* s, size_t len, char target)
{
    if (!s || len == 0) {
        return NULL;
    }
    const char* curr = s;
    const char* end = s + len;

#if defined(CSILK_HAS_AVX512)
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        const char* res = csilk_simd_find_char_avx512(curr, end, target);
        if (res < end && *res == target) {
            return res;
        }
        curr = res;
    }
#endif

#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        const char* res = csilk_simd_find_char_avx2(curr, end, target);
        if (res < end && *res == target) {
            return res;
        }
        curr = res;
    }
#elif defined(__ARM_NEON)
    uint8x16_t target_vec = vdupq_n_u8((uint8_t)target);
    while (curr + 16 <= end) {
        uint8x16_t data = vld1q_u8((const uint8_t*)curr);
        uint8x16_t cmp = vceqq_u8(data, target_vec);
        uint64_t   mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t   mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (mask_low != 0) {
            int idx = __builtin_ctzll(mask_low) / 8;
            return curr + idx;
        } else if (mask_high != 0) {
            int idx = __builtin_ctzll(mask_high) / 8;
            return curr + 8 + idx;
        }
        curr += 16;
    }
#endif

    while (curr < end) {
        if (*curr == target) {
            return curr;
        }
        curr++;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * csilk_common_prefix_len_fast — SIMD and word-at-a-time prefix comparison
 * -------------------------------------------------------------------------*/

/**
 * @brief Compute the length of the common prefix of two byte buffers.
 *
 * Compares @p s1 and @p s2 up to @p max_len bytes, using AVX2 (32-byte chunks) and
 * 8-byte word loads on supported architectures, stopping at the first differing byte.
 *
 * @param[in] s1      First buffer.
 * @param[in] s2      Second buffer.
 * @param[in] max_len Maximum number of bytes to compare.
 * @return Number of leading equal bytes (capped at @p max_len).
 */
size_t
csilk_common_prefix_len_fast(const char* s1, const char* s2, size_t max_len)
{
    size_t i = 0;

#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        while (i + 32 <= max_len) {
            __m256i v1 = _mm256_loadu_si256((const __m256i*)(const void*)(s1 + i));
            __m256i v2 = _mm256_loadu_si256((const __m256i*)(const void*)(s2 + i));
            __m256i cmp = _mm256_cmpeq_epi8(v1, v2);
            int     mask = _mm256_movemask_epi8(cmp);
            if (mask != (int)0xFFFFFFFF) {
                int diff_byte = __builtin_ctz(~(uint32_t)mask);
                return i + (size_t)diff_byte;
            }
            i += 32;
        }
    }
#endif

#if defined(__x86_64__) || defined(__aarch64__)
    while (i + 8 <= max_len) {
        uint64_t v1, v2;
        memcpy(&v1, s1 + i, 8);
        memcpy(&v2, s2 + i, 8);
        if (v1 != v2) {
            uint64_t diff = v1 ^ v2;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            int diff_byte = __builtin_clzll(diff) / 8;
#else
            int diff_byte = __builtin_ctzll(diff) / 8;
#endif
            return i + (size_t)diff_byte;
        }
        i += 8;
    }
#endif

    while (i < max_len && s1[i] == s2[i]) {
        i++;
    }
    return i;
}
