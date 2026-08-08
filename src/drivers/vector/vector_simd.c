#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vector_internal.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

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
