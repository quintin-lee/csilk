/**
 * @file test_oom.c
 * @brief OOM (Out Of Memory) simulation tests.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

#ifndef TEST_OOM
#error "TEST_OOM must be defined for this test"
#endif

void
test_oom_arena()
{
    printf("Testing Arena OOM...\n");
    g_oom_fail_after = 0; /* Fail on first malloc */
    g_oom_count = 0;

    csilk_arena_t* arena = csilk_arena_new(1024);
    assert(arena == nullptr);

    g_oom_fail_after = 1; /* Succeed on arena struct, fail on first chunk */
    g_oom_count = 0;
    arena = csilk_arena_new(1024);
    assert(arena != nullptr);
    void* ptr = csilk_arena_alloc(arena, 10);
    assert(ptr == nullptr);
    csilk_arena_free(arena);

    g_oom_fail_after = -1; /* Disable OOM */
}

void
test_oom_context()
{
    printf("Testing Context storage OOM...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    g_oom_fail_after = 0;
    g_oom_count = 0;
    csilk_set(c, "test", (void*)0x1);
    assert(csilk_get(c, "test") == nullptr);

    g_oom_fail_after = -1;
    csilk_test_ctx_free(c);
}

void
test_oom_header_map()
{
    printf("Testing Header Map OOM...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    g_oom_fail_after = 1; /* Struct allocated, but header entry fails */
    g_oom_count = 0;
    csilk_set_header(ctx, "Key", "Value");
    assert(csilk_get_header(ctx, "Key") == nullptr);

    g_oom_fail_after = -1;
    csilk_test_ctx_free(ctx);
}

#ifndef CSILK_USE_URING
void
test_oom_ws_send()
{
    printf("Testing WS send OOM...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_io_loop_t* loop = csilk_io_default_loop();
    uv_tcp_t         client;
    uv_tcp_init(loop, &client);
    _csilk_set_internal_client(ctx, &client);

    g_oom_fail_after = 0;
    g_oom_count = 0;
    csilk_ws_send(ctx, (uint8_t*)"payload", 7, 1);
    /* Should not crash */

    g_oom_fail_after = -1;
    csilk_test_ctx_free(ctx);
}
#endif

int
main()
{
    test_oom_arena();
    test_oom_context();
    test_oom_header_map();
#ifndef CSILK_USE_URING
    test_oom_ws_send();
#endif
    printf("All OOM tests passed!\n");
    return 0;
}
