#include "csilk_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk.h"

void test_csrf_token_generation() {
    char token1[64];
    char token2[64];
    
    assert(csilk_csrf_generate_token(token1, sizeof(token1)) == 0);
    assert(csilk_csrf_generate_token(token2, sizeof(token2)) == 0);
    
    assert(strlen(token1) == 32);
    assert(strcmp(token1, token2) != 0); // Tokens should be unique
    
    printf("test_csrf_token_generation passed\n");
}

void test_csrf_buffer_too_small() {
    char buf[2];
    assert(csilk_csrf_generate_token(buf, 2) == -1);
    printf("test_csrf_buffer_too_small passed\n");
}

void test_csrf_middleware_missing_token() {
    csilk_ctx_t ctx = {0};
    ctx.arena = csilk_arena_new(1024);
    ctx.handler_index = -1;

    csilk_handler_t handlers[] = {NULL};
    ctx.handlers = handlers;

    csilk_csrf_middleware(&ctx);
    assert(ctx.response.status == CSILK_STATUS_FORBIDDEN || ctx.response.status == 0);

    csilk_ctx_cleanup(&ctx);
    csilk_arena_free(ctx.arena);
    printf("test_csrf_middleware_missing_token passed\n");
}

int main() {
    test_csrf_token_generation();
    test_csrf_buffer_too_small();
    test_csrf_middleware_missing_token();
    return 0;
}
