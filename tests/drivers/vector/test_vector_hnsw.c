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

    for (size_t i = 0; i < dim; i++) {
        v1[i] = 1.0f;
        v2[i] = (i % 2 == 0) ? 1.0f : 0.0f;
    }

    assert(csilk_hnsw_insert(idx, "doc_1", v1) == 0);
    assert(csilk_hnsw_insert(idx, "doc_2", v2) == 0);

    char** res_ids = nullptr;
    float* res_scores = nullptr;
    size_t count = 0;

    int res = csilk_hnsw_search(idx, v1, 2, &res_ids, &res_scores, &count);
    assert(res == 0);
    assert(count == 2);
    assert(res_ids != nullptr);
    assert(strcmp(res_ids[0], "doc_1") == 0);

    for (size_t i = 0; i < count; i++) {
        free(res_ids[i]);
    }
    free(res_ids);
    free(res_scores);
    free(v1);
    free(v2);

    csilk_hnsw_index_free(idx);
    printf("test_hnsw_index_insertion_and_search passed\n");
}

int
main(void)
{
    test_hnsw_index_insertion_and_search();
    printf("All test_vector_hnsw tests passed successfully!\n");
    return 0;
}
