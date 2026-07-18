#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/mvcc_cache.h"

int
main(void)
{
    printf("Testing Epoch-based RCU / MVCC Lock-Free Cache...\n");

    csilk_mvcc_cache_t* cache = csilk_mvcc_cache_new(16);
    assert(cache != NULL);

    const char* k1 = "user:1001";
    const char* v1 = "Alice";
    assert(csilk_mvcc_cache_set(cache, k1, v1, strlen(v1) + 1) == 0);

    size_t      len = 0;
    const char* read_v1 = csilk_mvcc_cache_get(cache, k1, &len);
    assert(read_v1 != NULL);
    assert(strcmp(read_v1, "Alice") == 0);
    assert(len == strlen(v1) + 1);

    /* Test MVCC update (atomic RCU pointer swap) */
    const char* v2 = "Alice_Updated";
    assert(csilk_mvcc_cache_set(cache, k1, v2, strlen(v2) + 1) == 0);

    const char* read_v2 = csilk_mvcc_cache_get(cache, k1, &len);
    assert(read_v2 != NULL);
    assert(strcmp(read_v2, "Alice_Updated") == 0);

    csilk_mvcc_cache_free(cache);
    printf("test_mvcc_cache: PASS\n");
    return 0;
}
