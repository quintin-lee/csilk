#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/drivers/vector.h"

void* csilk_aligned_alloc(size_t alignment, size_t size);
void  csilk_aligned_free(void* ptr);
float csilk_simd_cosine_distance(const float* a, const float* b, size_t dim);
float csilk_simd_l2_distance(const float* a, const float* b, size_t dim);

static void
test_simd_aligned_alloc_and_distance(void)
{
    size_t dim = 128;
    float* v1 = (float*)csilk_aligned_alloc(32, dim * sizeof(float));
    float* v2 = (float*)csilk_aligned_alloc(32, dim * sizeof(float));

    assert(v1 != nullptr);
    assert(v2 != nullptr);
    assert(((uintptr_t)v1 % 32) == 0);
    assert(((uintptr_t)v2 % 32) == 0);

    for (size_t i = 0; i < dim; i++) {
        v1[i] = 1.0f;
        v2[i] = (i % 2 == 0) ? 1.0f : 0.0f;
    }

    float cos_dist = csilk_simd_cosine_distance(v1, v2, dim);
    assert(cos_dist >= 0.0f && cos_dist <= 1.0f);

    float l2_dist = csilk_simd_l2_distance(v1, v2, dim);
    assert(l2_dist > 0.0f);

    csilk_aligned_free(v1);
    csilk_aligned_free(v2);
    printf("test_simd_aligned_alloc_and_distance passed\n");
}

int
main(void)
{
    test_simd_aligned_alloc_and_distance();
    printf("All test_vector_simd tests passed successfully!\n");
    return 0;
}
