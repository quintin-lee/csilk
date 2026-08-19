#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"

/* -------------------------------------------------------------------------- */
/* Test 1: Release Idempotency & Double-Free Protection                        */
/* -------------------------------------------------------------------------- */
static void
test_release_idempotency(void)
{
    printf("Testing body release idempotency and double-free protection...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    /* Initial state: NONE */
    assert(csilk_get_response_body_ownership(ctx) == CSILK_OWN_NONE);
    for (int i = 0; i < 10; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_len == 0);
        assert(ctx->response.body_capacity == 0);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* HEAP body release idempotency */
    char* heap_buf = strdup("heap allocated payload");
    csilk_set_response_body_ex(ctx, heap_buf, strlen(heap_buf), CSILK_OWN_HEAP);
    assert(ctx->response.body_ownership == CSILK_OWN_HEAP);
    for (int i = 0; i < 5; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_len == 0);
        assert(ctx->response.body_capacity == 0);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* TRANSFER body release idempotency */
    char* transfer_buf = strdup("transferred payload");
    csilk_set_response_body_ex(ctx, transfer_buf, strlen(transfer_buf), CSILK_OWN_TRANSFER);
    assert(ctx->response.body_ownership == CSILK_OWN_TRANSFER);
    for (int i = 0; i < 5; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* POOL body release idempotency */
    char* pool_buf = csilk_set_response_body_pooled(ctx, 32 * 1024);
    assert(pool_buf != NULL);
    assert(ctx->response.body_ownership == CSILK_OWN_POOL);
    for (int i = 0; i < 5; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* BORROWED body release idempotency */
    const char* borrowed_str = "constant string";
    csilk_set_response_body_ex(ctx, borrowed_str, strlen(borrowed_str), CSILK_OWN_BORROWED);
    assert(ctx->response.body_ownership == CSILK_OWN_BORROWED);
    for (int i = 0; i < 5; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* ARENA body release idempotency */
    char* arena_buf = csilk_arena_strdup(ctx->arena, "arena payload");
    csilk_set_response_body_ex(ctx, arena_buf, strlen(arena_buf), CSILK_OWN_ARENA);
    assert(ctx->response.body_ownership == CSILK_OWN_ARENA);
    for (int i = 0; i < 5; i++) {
        csilk_response_body_release(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    /* Request body release idempotency */
    char* req_heap = strdup("req heap");
    ctx->request.body = req_heap;
    ctx->request.body_len = strlen(req_heap);
    ctx->request.body_ownership = CSILK_OWN_HEAP;
    for (int i = 0; i < 5; i++) {
        csilk_request_body_release(ctx);
        assert(ctx->request.body == NULL);
        assert(ctx->request.body_len == 0);
        assert(ctx->request.body_ownership == CSILK_OWN_NONE);
    }

    csilk_test_ctx_free(ctx);
    printf("test_release_idempotency: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Sequential Body Overwrites Without Leaks or Double-Frees           */
/* -------------------------------------------------------------------------- */
static void
test_sequential_body_overwrites(void)
{
    printf("Testing sequential body overwrites across different ownerships...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    for (int cycle = 0; cycle < 100; cycle++) {
        /* Step 1: HEAP */
        char* heap_buf = strdup("heap 1");
        csilk_set_response_body_ex(ctx, heap_buf, strlen(heap_buf), CSILK_OWN_HEAP);
        assert(ctx->response.body_ownership == CSILK_OWN_HEAP);
        assert(strcmp(csilk_get_response_body(ctx, NULL), "heap 1") == 0);

        /* Step 2: POOL (must safely free heap 1) */
        char* pool_buf = csilk_set_response_body_pooled(ctx, 40 * 1024);
        assert(pool_buf != NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_POOL);
        memcpy(pool_buf, "pool data", 9);

        /* Step 3: ARENA (must safely recycle pool buffer) */
        char* arena_buf = csilk_arena_strdup(ctx->arena, "arena data");
        csilk_set_response_body_ex(ctx, arena_buf, strlen(arena_buf), CSILK_OWN_ARENA);
        assert(ctx->response.body_ownership == CSILK_OWN_ARENA);

        /* Step 4: BORROWED (arena stays alive until ctx reset) */
        const char* static_msg = "borrowed static text";
        csilk_set_response_body_ex(ctx, static_msg, strlen(static_msg), CSILK_OWN_BORROWED);
        assert(ctx->response.body_ownership == CSILK_OWN_BORROWED);

        /* Step 5: TRANSFER (must safely skip freeing borrowed) */
        char* transfer_buf = strdup("transferred buffer");
        csilk_set_response_body_ex(ctx, transfer_buf, strlen(transfer_buf), CSILK_OWN_TRANSFER);
        assert(ctx->response.body_ownership == CSILK_OWN_TRANSFER);

        /* Step 6: HEAP (must safely free transfer buffer) */
        char* heap_buf2 = strdup("heap 2");
        csilk_set_response_body_ex(ctx, heap_buf2, strlen(heap_buf2), CSILK_OWN_HEAP);
        assert(ctx->response.body_ownership == CSILK_OWN_HEAP);

        /* Cleanup end of cycle */
        csilk_ctx_cleanup(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    }

    csilk_test_ctx_free(ctx);
    csilk_body_pool_cleanup();
    printf("test_sequential_body_overwrites: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Borrowed Body Lifetime (Stack & Static Data)                       */
/* -------------------------------------------------------------------------- */
static void
test_borrowed_body_lifetime(void)
{
    printf("Testing borrowed body lifetime and non-interference...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    /* Stack allocated buffer with guard sentinels */
    char stack_buf[128];
    memset(stack_buf, 'Z', sizeof(stack_buf));
    strcpy(stack_buf, "{\"status\":\"ok\",\"code\":200}");
    size_t stack_len = strlen(stack_buf);

    /* Test csilk_json_string */
    csilk_json_string(ctx, 200, stack_buf);
    assert(ctx->response.status == 200);
    assert(ctx->response.body_ownership == CSILK_OWN_BORROWED);
    assert(ctx->response.body == stack_buf);
    assert(ctx->response.body_len == stack_len);

    /* Context cleanup must not touch stack_buf */
    csilk_ctx_cleanup(ctx);
    assert(strcmp(stack_buf, "{\"status\":\"ok\",\"code\":200}") == 0);
    assert(ctx->response.body == NULL);
    assert(ctx->response.body_ownership == CSILK_OWN_NONE);

    /* Test csilk_set_response_body with managed=0 */
    csilk_set_response_body(ctx, stack_buf, stack_len, 0);
    assert(ctx->response.body_ownership == CSILK_OWN_BORROWED);
    assert(ctx->response.body == stack_buf);

    csilk_ctx_cleanup(ctx);
    assert(strcmp(stack_buf, "{\"status\":\"ok\",\"code\":200}") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_borrowed_body_lifetime: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Response Producer APIs Ownership Guarantees                        */
/* -------------------------------------------------------------------------- */
static void
test_response_producer_apis(void)
{
    printf("Testing response producer APIs ownership guarantees...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    /* 1. csilk_string with arena */
    csilk_string(ctx, 200, "Hello World Plain Text");
    assert(ctx->response.status == 200);
    assert(ctx->response.body_ownership == CSILK_OWN_ARENA);
    assert(strcmp(csilk_get_response_body(ctx, NULL), "Hello World Plain Text") == 0);

    /* 2. csilk_string with NULL text */
    csilk_string(ctx, 204, NULL);
    assert(ctx->response.status == 204);
    assert(ctx->response.body == NULL);
    assert(ctx->response.body_ownership == CSILK_OWN_NONE);

    /* 3. csilk_json */
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "message", "hello json");
    csilk_json(ctx, 200, obj);
    assert(ctx->response.body_ownership == CSILK_OWN_HEAP);
    assert(strstr(csilk_get_response_body(ctx, NULL), "hello json") != NULL);

    /* 4. csilk_json_string */
    csilk_json_string(ctx, 201, "{\"created\":true}");
    assert(ctx->response.body_ownership == CSILK_OWN_BORROWED);
    assert(strcmp(csilk_get_response_body(ctx, NULL), "{\"created\":true}") == 0);

    /* 5. csilk_json_error (short arena fast-path) */
    csilk_json_error(ctx, 400, "Invalid parameter");
    assert(ctx->response.status == 400);
    assert(ctx->response.body_ownership == CSILK_OWN_ARENA);
    assert(strstr(csilk_get_response_body(ctx, NULL), "Invalid parameter") != NULL);

    /* 6. csilk_json_error (large heap fallback) */
    char large_err[512];
    memset(large_err, 'E', 500);
    large_err[500] = '\0';
    csilk_json_error(ctx, 500, large_err);
    assert(ctx->response.status == 500);
    assert(ctx->response.body_ownership == CSILK_OWN_HEAP);
    assert(strstr(csilk_get_response_body(ctx, NULL), "EEEE") != NULL);

    /* 7. csilk_set_file_response releases existing body */
    char* heap_prior = strdup("prior body before sendfile");
    csilk_set_response_body_ex(ctx, heap_prior, strlen(heap_prior), CSILK_OWN_HEAP);
    assert(ctx->response.body_ownership == CSILK_OWN_HEAP);
    csilk_set_file_response(ctx, 42, 0, 1024);
    assert(ctx->file_fd == 42);
    assert(ctx->response.body == NULL);
    assert(ctx->response.body_ownership == CSILK_OWN_NONE);
    ctx->file_fd = -1; /* Avoid trying to close fd 42 on mock cleanup */

    csilk_test_ctx_free(ctx);
    printf("test_response_producer_apis: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: Pool Body Size-Class Recycling                                     */
/* -------------------------------------------------------------------------- */
static void
test_pool_body_recycling(void)
{
    printf("Testing pool body allocation and recycling via unified ownership...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    const size_t tiers[] = {10 * 1024, 80 * 1024, 200 * 1024, 450 * 1024, 900 * 1024};
    const size_t expected_caps[] = {64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024, 1024 * 1024};

    for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        char* buf1 = csilk_set_response_body_pooled(ctx, tiers[i]);
        assert(buf1 != NULL);
        assert(ctx->response.body_capacity == expected_caps[i]);
        assert(ctx->response.body_ownership == CSILK_OWN_POOL);
        memset(buf1, 0xCC, 256);

        /* Cleanup returns to size class pool */
        csilk_ctx_cleanup(ctx);
        assert(ctx->response.body == NULL);
        assert(ctx->response.body_ownership == CSILK_OWN_NONE);

        /* Re-allocate should hit cache in the same thread */
        char* buf2 = csilk_set_response_body_pooled(ctx, tiers[i]);
        assert(buf2 == buf1);
        assert(ctx->response.body_capacity == expected_caps[i]);
        assert(ctx->response.body_ownership == CSILK_OWN_POOL);

        csilk_ctx_cleanup(ctx);
    }

    csilk_test_ctx_free(ctx);
    csilk_body_pool_cleanup();
    printf("test_pool_body_recycling: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Main Runner                                                                */
/* -------------------------------------------------------------------------- */
int
main(void)
{
    printf("=== Running Response Body Unified Ownership Tests ===\n\n");
    test_release_idempotency();
    test_sequential_body_overwrites();
    test_borrowed_body_lifetime();
    test_response_producer_apis();
    test_pool_body_recycling();
    printf("\n=== All Response Body Ownership Tests Passed! ===\n");
    return 0;
}
