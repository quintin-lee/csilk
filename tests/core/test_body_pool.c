#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"

/* Test 1: Size class allocation and cache reuse */
static void
test_size_classes_and_reuse(void)
{
    printf("Testing size class allocation and reuse...\n");

    const size_t test_sizes[] = {
        1000,       /* Fits in 64KB (Tier 0) */
        64 * 1024,  /* Exact 64KB (Tier 0) */
        65 * 1024,  /* Fits in 128KB (Tier 1) */
        128 * 1024, /* Exact 128KB (Tier 1) */
        200 * 1024, /* Fits in 256KB (Tier 2) */
        256 * 1024, /* Exact 256KB (Tier 2) */
        400 * 1024, /* Fits in 512KB (Tier 3) */
        512 * 1024, /* Exact 512KB (Tier 3) */
        800 * 1024, /* Fits in 1MB (Tier 4) */
        1024 * 1024 /* Exact 1MB (Tier 4) */
    };
    const size_t expected_caps[] = {64 * 1024,
                                    64 * 1024,
                                    128 * 1024,
                                    128 * 1024,
                                    256 * 1024,
                                    256 * 1024,
                                    512 * 1024,
                                    512 * 1024,
                                    1024 * 1024,
                                    1024 * 1024};

    for (size_t i = 0; i < sizeof(test_sizes) / sizeof(test_sizes[0]); i++) {
        size_t cap1 = 0;
        void*  ptr1 = csilk_body_alloc(test_sizes[i], &cap1);
        assert(ptr1 != NULL);
        assert(cap1 == expected_caps[i]);
        assert(cap1 >= test_sizes[i]);

        /* Write data to ensure writable and clean */
        memset(ptr1, (int)(i + 1), 512);

        /* Return to pool */
        csilk_body_free(ptr1, cap1);

        /* Re-allocate same size: MUST get the same pointer back (cache hit) */
        size_t cap2 = 0;
        void*  ptr2 = csilk_body_alloc(test_sizes[i], &cap2);
        assert(ptr2 == ptr1);
        assert(cap2 == cap1);

        csilk_body_free(ptr2, cap2);
    }

    csilk_body_pool_cleanup();
    printf("test_size_classes_and_reuse: PASS\n");
}

/* Test 2: Pool capacity limit (max 8 per tier) */
static void
test_pool_tier_capacity_limit(void)
{
    printf("Testing pool tier capacity limits...\n");

    void*  ptrs[12];
    size_t caps[12];

    /* Allocate 12 buffers of 64KB */
    for (int i = 0; i < 12; i++) {
        ptrs[i] = csilk_body_alloc(64 * 1024, &caps[i]);
        assert(ptrs[i] != NULL);
        assert(caps[i] == 64 * 1024);
    }

    /* Return all 12: 8 should be cached, 4 freed */
    for (int i = 0; i < 12; i++) {
        csilk_body_free(ptrs[i], caps[i]);
    }

    /* Allocate 8: all should be from cache (in reverse LIFO order) */
    void* cached[8];
    for (int i = 0; i < 8; i++) {
        size_t cap = 0;
        cached[i] = csilk_body_alloc(64 * 1024, &cap);
        assert(cached[i] != NULL);
        assert(cap == 64 * 1024);
    }

    for (int i = 0; i < 8; i++) {
        csilk_body_free(cached[i], 64 * 1024);
    }

    csilk_body_pool_cleanup();
    printf("test_pool_tier_capacity_limit: PASS\n");
}

/* Test 3: Unpooled large requests (> 1MB) */
static void
test_unpooled_large_requests(void)
{
    printf("Testing unpooled large body allocations (> 1MB)...\n");

    const size_t large_sizes[] = {
        2 * 1024 * 1024, /* 2MB */
        5 * 1024 * 1024, /* 5MB */
        10 * 1024 * 1024 /* 10MB */
    };

    for (size_t i = 0; i < sizeof(large_sizes) / sizeof(large_sizes[0]); i++) {
        size_t cap = 0;
        void*  ptr = csilk_body_alloc(large_sizes[i], &cap);
        assert(ptr != NULL);
        assert(cap == large_sizes[i]);

        /* Write data */
        memset(ptr, 0xAB, 1024);

        /* Return: unpooled sizes are directly freed */
        csilk_body_free(ptr, cap);
    }

    csilk_body_pool_cleanup();
    printf("test_unpooled_large_requests: PASS\n");
}

/* Test 4: Body reallocation and growth */
static void
test_body_reallocation_growth(void)
{
    printf("Testing body buffer incremental growth...\n");

    size_t cap = 0;
    char*  buf = (char*)csilk_body_alloc(10 * 1024, &cap);
    assert(buf != NULL);
    assert(cap == 64 * 1024);
    strcpy(buf, "Hello, Csilk!");
    size_t cur_len = strlen(buf);

    /* Grow within existing capacity (64KB): should be NO-OP reuse */
    size_t cap2 = 0;
    char*  buf2 = (char*)csilk_body_realloc(buf, cur_len, cap, 30 * 1024, &cap2);
    assert(buf2 == buf);
    assert(cap2 == cap);
    assert(strcmp(buf2, "Hello, Csilk!") == 0);

    /* Grow to 100KB (crosses into 128KB tier) */
    size_t cap3 = 0;
    char*  buf3 = (char*)csilk_body_realloc(buf2, cur_len, cap2, 100 * 1024, &cap3);
    assert(buf3 != NULL);
    assert(cap3 == 128 * 1024);
    assert(strcmp(buf3, "Hello, Csilk!") == 0);

    /* Grow to 200KB (crosses into 256KB tier) */
    size_t cap4 = 0;
    char*  buf4 = (char*)csilk_body_realloc(buf3, cur_len, cap3, 200 * 1024, &cap4);
    assert(buf4 != NULL);
    assert(cap4 == 256 * 1024);
    assert(strcmp(buf4, "Hello, Csilk!") == 0);

    /* Grow to 2MB (crosses into unpooled heap) */
    size_t cap5 = 0;
    char*  buf5 = (char*)csilk_body_realloc(buf4, cur_len, cap4, 2 * 1024 * 1024, &cap5);
    assert(buf5 != NULL);
    assert(cap5 == 2 * 1024 * 1024);
    assert(strcmp(buf5, "Hello, Csilk!") == 0);

    csilk_body_free(buf5, cap5);
    csilk_body_pool_cleanup();
    printf("test_body_reallocation_growth: PASS\n");
}

/* Test 5: Context cleanup recycling */
static void
test_context_cleanup_recycling(void)
{
    printf("Testing context cleanup recycling into size-class pool...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    /* 1. Allocate request body via size-class pool */
    size_t req_cap = 0;
    char*  req_body = (char*)csilk_body_alloc(120 * 1024, &req_cap);
    assert(req_body != NULL);
    assert(req_cap == 128 * 1024);
    ctx->request.body = req_body;
    ctx->request.body_len = 120 * 1024;
    ctx->request.body_capacity = req_cap;
    ctx->request.body_ownership = CSILK_OWN_POOL;

    /* 2. Allocate response body via csilk_set_response_body_pooled */
    char* resp_body = csilk_set_response_body_pooled(ctx, 250 * 1024);
    assert(resp_body != NULL);
    assert(ctx->response.body_capacity == 256 * 1024);
    assert(ctx->response.body_ownership == CSILK_OWN_POOL);

    /* 3. Run cleanup */
    csilk_ctx_cleanup(ctx);

    /* Both buffers should now be in their respective size-class pools */
    size_t re_req_cap = 0;
    char*  re_req = (char*)csilk_body_alloc(120 * 1024, &re_req_cap);
    assert(re_req == req_body);
    assert(re_req_cap == 128 * 1024);

    size_t re_resp_cap = 0;
    char*  re_resp = (char*)csilk_body_alloc(250 * 1024, &re_resp_cap);
    assert(re_resp == resp_body);
    assert(re_resp_cap == 256 * 1024);

    csilk_body_free(re_req, re_req_cap);
    csilk_body_free(re_resp, re_resp_cap);

    csilk_test_ctx_free(ctx);
    csilk_body_pool_cleanup();
    printf("test_context_cleanup_recycling: PASS\n");
}

/* Test 6: Multi-threaded isolation (no cross-thread locking or corruption) */
static void*
worker_thread_func(void* arg)
{
    (void)arg;
    for (int iter = 0; iter < 100; iter++) {
        size_t cap = 0;
        char*  buf = (char*)csilk_body_alloc(60 * 1024, &cap);
        assert(buf != NULL);
        assert(cap == 64 * 1024);
        buf[0] = 'A';
        buf[60 * 1024 - 1] = 'Z';
        csilk_body_free(buf, cap);
    }
    csilk_body_pool_cleanup();
    return NULL;
}

static void
test_multithreaded_isolation(void)
{
    printf("Testing multi-threaded worker pool isolation...\n");

    const int THREAD_COUNT = 4;
    pthread_t threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, worker_thread_func, NULL);
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("test_multithreaded_isolation: PASS\n");
}

int
main(void)
{
    printf("=== Running HTTP Body Size-Class Cache Tests ===\n\n");
    test_size_classes_and_reuse();
    test_pool_tier_capacity_limit();
    test_unpooled_large_requests();
    test_body_reallocation_growth();
    test_context_cleanup_recycling();
    test_multithreaded_isolation();
    printf("\n=== All HTTP Body Size-Class Cache Tests Passed! ===\n");
    return 0;
}
