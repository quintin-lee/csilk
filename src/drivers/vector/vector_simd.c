/**
 * @file vector_simd.c
 * @brief SIMD-aware vector distance kernels and aligned allocators.
 *
 * Implements cosine, L2 (squared Euclidean) and dot-product distance kernels
 * used by the HNSW index, plus a portable aligned memory allocator.  AVX2
 * support is detected at runtime via csilk_simd_has_avx2() (compiled paths
 * are available when __AVX2__ is defined).
 *
 * @copyright MIT License
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vector_internal.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

/**
 * @brief Report whether the CPU supports AVX2.
 *
 * When compiled with __AVX2__ the answer is a compile-time yes; otherwise on
 * x86-64 it uses the compiler's runtime CPU feature detection, and on other
 * architectures it always returns 0.
 *
 * @return 1 if AVX2 is available, 0 otherwise. */
int
csilk_simd_has_avx2(void)
{
#if defined(__AVX2__)
    return 1;
#elif defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return 0;
#endif
#else
    return 0;
#endif
}

/**
 * @brief Allocate @p size bytes aligned to @p alignment.
 *
 * On POSIX systems with _POSIX_C_SOURCE >= 200112L this uses posix_memalign.
 * Otherwise it overallocates and stores the original pointer immediately
 * before the returned region so csilk_aligned_free() can recover it.
 *
 * @param alignment Required alignment, a power of two (must be >= 1).
 * @param size      Number of usable bytes to allocate.
 * @return Aligned pointer, or NULL on allocation failure. */
void*
csilk_aligned_alloc(size_t alignment, size_t size)
{
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    void* ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
#else
    void* ptr = malloc(size + alignment + sizeof(void*));
    if (!ptr) {
        return NULL;
    }
    void** raw = (void**)(((uintptr_t)ptr + alignment + sizeof(void*)) & ~(alignment - 1));
    raw[-1] = ptr;
    return raw;
#endif
}

/**
 * @brief Free a pointer returned by csilk_aligned_alloc().
 *
 * Safe to call with NULL. Mirrors the allocation strategy used by
 * csilk_aligned_alloc() (a plain free() under posix_memalign, or freeing the
 * stored original pointer otherwise).
 *
 * @param ptr Pointer to release (may be NULL). */
void
csilk_aligned_free(void* ptr)
{
    if (!ptr) {
        return;
    }
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    free(ptr);
#else
    free(((void**)ptr)[-1]);
#endif
}

/**
 * @brief Compute 1 - cosine similarity as a distance in [0, 2].
 *
 * Returns 1.0f for degenerate inputs (NULL pointers or zero dimension, or a
 * zero-magnitude vector). Values are clamped to the valid cosine range before
 * the subtraction.
 *
 * @param a   First vector (must contain @p dim floats if non-NULL).
 * @param b   Second vector (must contain @p dim floats if non-NULL).
 * @param dim Number of components per vector.
 * @return Cosine distance (0 = identical, 2 = opposite). */
float
csilk_simd_cosine_distance(const float* a, const float* b, size_t dim)
{
    if (!a || !b || dim == 0) {
        return 1.0f;
    }

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        norm_a += (double)a[i] * (double)a[i];
        norm_b += (double)b[i] * (double)b[i];
    }

    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return 1.0f;
    }

    double cos_sim = dot / (sqrt(norm_a) * sqrt(norm_b));
    if (cos_sim > 1.0) {
        cos_sim = 1.0;
    }
    if (cos_sim < -1.0) {
        cos_sim = -1.0;
    }
    return (float)(1.0 - cos_sim);
}

/**
 * @brief Compute the squared L2 (Euclidean) distance between two vectors.
 *
 * Returns 0.0f for degenerate inputs (NULL pointers or zero dimension).
 * Note this is the un-squared sum of squared differences, not the Euclidean
 * norm.
 *
 * @param a   First vector (must contain @p dim floats if non-NULL).
 * @param b   Second vector (must contain @p dim floats if non-NULL).
 * @param dim Number of components per vector.
 * @return Squared Euclidean distance. */
float
csilk_simd_l2_distance(const float* a, const float* b, size_t dim)
{
    if (!a || !b || dim == 0) {
        return 0.0f;
    }

    double dist = 0.0;
    for (size_t i = 0; i < dim; i++) {
        double diff = (double)a[i] - (double)b[i];
        dist += diff * diff;
    }
    return (float)dist;
}

/**
 * @brief Compute the negative dot product, used as a similarity score.
 *
 * Returns 0.0f for degenerate inputs (NULL pointers or zero dimension). The
 * result is negated so that smaller values correspond to "more similar"
 * vectors, matching the minimising convention of csilk_hnsw_search().
 *
 * @param a   First vector (must contain @p dim floats if non-NULL).
 * @param b   Second vector (must contain @p dim floats if non-NULL).
 * @param dim Number of components per vector.
 * @return Negated dot product. */
float
csilk_simd_dot_product(const float* a, const float* b, size_t dim)
{
    if (!a || !b || dim == 0) {
        return 0.0f;
    }

    double dot = 0.0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
    }
    return (float)(-dot);
}
