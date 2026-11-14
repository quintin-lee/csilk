#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/drivers/vector.h"

typedef struct csilk_hnsw_index_s csilk_hnsw_index_t;
csilk_hnsw_index_t*               csilk_hnsw_index_new(size_t dim, int metric);
void                              csilk_hnsw_index_free(csilk_hnsw_index_t* index);
int csilk_hnsw_insert(csilk_hnsw_index_t* index, const char* doc_id, const float* vector);
int csilk_hnsw_search(csilk_hnsw_index_t* index,
                      const float*        query_vector,
                      size_t              top_k,
                      char***             out_doc_ids,
                      float**             out_scores,
                      size_t*             out_count);

static void
test_hnsw_index_insertion_and_search(void)
{
    size_t              dim = 64;
    csilk_hnsw_index_t* idx = csilk_hnsw_index_new(dim, 0);
    assert(idx != nullptr);

    float* v1 = calloc(dim, sizeof(float));
    float* v2 = calloc(dim, sizeof(float));
    float* v3 = calloc(dim, sizeof(float));

    for (size_t i = 0; i < dim; i++) {
        v1[i] = 1.0f;
        v2[i] = (i % 2 == 0) ? 1.0f : 0.0f;
        v3[i] = -1.0f; /* opposite direction — farthest from v1 */
    }

    /* Insert farthest first, closest last to verify true Top-K ranking */
    assert(csilk_hnsw_insert(idx, "doc_far", v3) == 0);
    assert(csilk_hnsw_insert(idx, "doc_2", v2) == 0);
    assert(csilk_hnsw_insert(idx, "doc_1", v1) == 0);

    char** res_ids = nullptr;
    float* res_scores = nullptr;
    size_t count = 0;

    int res = csilk_hnsw_search(idx, v1, 2, &res_ids, &res_scores, &count);
    assert(res == 0);
    assert(count == 2);
    assert(res_ids != nullptr);
    assert(strcmp(res_ids[0], "doc_1") == 0);
    assert(strcmp(res_ids[1], "doc_2") == 0);
    assert(res_scores[0] <= res_scores[1]);

    for (size_t i = 0; i < count; i++) {
        free(res_ids[i]);
    }
    free(res_ids);
    free(res_scores);
    free(v1);
    free(v2);
    free(v3);

    csilk_hnsw_index_free(idx);
    printf("test_hnsw_index_insertion_and_search passed\n");
}

static float
exact_l2_sq(const float* a, const float* b, size_t dim)
{
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

static void
test_hnsw_multilayer_recall_and_l2(void)
{
    enum { N = 400, DIM = 32, K = 5 };
    csilk_hnsw_index_t* idx = csilk_hnsw_index_new(DIM, 1); /* L2 metric */
    assert(idx != nullptr);

    float*   vectors = calloc((size_t)N * DIM, sizeof(float));
    char     id_buf[32];
    uint64_t seed = 0x123456789abcdef0ULL;

    for (int i = 0; i < N; i++) {
        for (int d = 0; d < DIM; d++) {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            float val = (float)((seed * 2685821657736338717ULL) & 0xFFFF) / 65535.0f;
            vectors[(size_t)i * DIM + (size_t)d] = val * 2.0f - 1.0f;
        }
        snprintf(id_buf, sizeof(id_buf), "vec_%d", i);
        assert(csilk_hnsw_insert(idx, id_buf, &vectors[(size_t)i * DIM]) == 0);
    }

    /* Verify multiple queries against exact brute-force Top-K ground truth. */
    int hits = 0;
    int queries = 10;
    for (int q = 0; q < queries; q++) {
        float query[DIM];
        for (int d = 0; d < DIM; d++) {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            float val = (float)((seed * 2685821657736338717ULL) & 0xFFFF) / 65535.0f;
            query[d] = val * 2.0f - 1.0f;
        }

        /* Exact best match */
        int   best_idx = 0;
        float best_d = exact_l2_sq(query, &vectors[0], DIM);
        for (int i = 1; i < N; i++) {
            float d = exact_l2_sq(query, &vectors[(size_t)i * DIM], DIM);
            if (d < best_d) {
                best_d = d;
                best_idx = i;
            }
        }
        snprintf(id_buf, sizeof(id_buf), "vec_%d", best_idx);

        char** res_ids = nullptr;
        float* res_scores = nullptr;
        size_t count = 0;
        assert(csilk_hnsw_search(idx, query, K, &res_ids, &res_scores, &count) == 0);
        assert(count == K);
        for (size_t i = 0; i < count; i++) {
            if (i > 0) {
                assert(res_scores[i - 1] <= res_scores[i]);
            }
            if (strcmp(res_ids[i], id_buf) == 0) {
                hits++;
            }
            free(res_ids[i]);
        }
        free(res_ids);
        free(res_scores);
    }
    assert(hits >= 9);

    /* Also verify top_k > ef_search (ef_search=64, top_k=80). */
    char** res_ids = nullptr;
    float* res_scores = nullptr;
    size_t count = 0;
    assert(csilk_hnsw_search(idx, &vectors[0], 80, &res_ids, &res_scores, &count) == 0);
    assert(count == 80);
    assert(strcmp(res_ids[0], "vec_0") == 0);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            assert(res_scores[i - 1] <= res_scores[i]);
        }
        free(res_ids[i]);
    }
    free(res_ids);
    free(res_scores);

    free(vectors);
    csilk_hnsw_index_free(idx);
    printf(
        "test_hnsw_multilayer_recall_and_l2 passed (top-1 in top-%d: %d/%d)\n", K, hits, queries);
}

int
main(void)
{
    test_hnsw_index_insertion_and_search();
    test_hnsw_multilayer_recall_and_l2();
    printf("All test_vector_hnsw tests passed successfully!\n");
    return 0;
}
