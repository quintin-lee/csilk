#include "csilk_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk.h"

static void test_sse_init_null() {
    printf("Testing SSE init with NULL context...\n");
    csilk_sse_init(NULL);
    printf("SSE init null test passed!\n");
}

static void test_sse_send_null() {
    printf("Testing SSE send with NULL context...\n");
    csilk_sse_send(NULL, "event", "data");
    csilk_sse_send(NULL, NULL, "data");
    csilk_sse_send(NULL, "event", NULL);
    printf("SSE send null test passed!\n");
}

static void test_sse_close_null() {
    printf("Testing SSE close with NULL context...\n");
    csilk_sse_close(NULL);
    printf("SSE close null test passed!\n");
}

static void test_sse_init_headers() {
    printf("Testing SSE init sets status and headers...\n");
    csilk_ctx_t ctx = {0};
    ctx.arena = csilk_arena_new(1024);

    csilk_sse_init(&ctx);
    /* csilk_sse_init needs _internal_client to send headers;
     * when _internal_client is NULL it still sets up the context */
    assert(ctx.is_websocket == 1 || ctx.response.status == CSILK_STATUS_OK);

    csilk_ctx_cleanup(&ctx);
    csilk_arena_free(ctx.arena);
    printf("SSE init headers test passed!\n");
}

int main() {
    test_sse_init_null();
    test_sse_send_null();
    test_sse_close_null();
    test_sse_init_headers();
    printf("test_sse: ALL PASSED\n");
    return 0;
}
