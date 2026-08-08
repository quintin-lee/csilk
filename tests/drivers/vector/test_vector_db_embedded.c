#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/drivers/vector.h"

static void
test_vector_db_embedded_factory(void)
{
    size_t             dim = 128;
    csilk_vector_db_t* db = csilk_vector_db_new_embedded(dim, 0);
    assert(db != nullptr);
    assert(csilk_simd_has_avx2() >= 0);

    float* vec = calloc(dim, sizeof(float));
    for (size_t i = 0; i < dim; i++) {
        vec[i] = 1.0f;
    }

    csilk_vector_point_t pt;
    pt.id = "doc_embedded_1";
    pt.vector = vec;
    pt.dimension = dim;
    pt.payload = nullptr;

    assert(csilk_vector_db_upsert(db, "default", &pt, 1) == 0);

    csilk_vector_search_response_t res;
    memset(&res, 0, sizeof(res));

    assert(csilk_vector_db_search(db, "default", vec, dim, 1, &res) == 0);
    assert(res.count == 1);
    assert(res.results != nullptr);
    assert(strcmp(res.results[0].id, "doc_embedded_1") == 0);

    csilk_vector_search_response_free(&res);
    free(vec);
    csilk_vector_db_free(db);
    printf("test_vector_db_embedded_factory passed\n");
}

int
main(void)
{
    test_vector_db_embedded_factory();
    printf("All test_vector_db_embedded tests passed successfully!\n");
    return 0;
}
