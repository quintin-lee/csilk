#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/ctx/ctx_internal.h"

// Fuzz test for csilk_parse_form_urlencoded and header operations

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) {
        return 0;
    }

    // Create null-terminated string
    char* input = malloc(size + 1);
    if (!input) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    csilk_ctx_t ctx;
    _csilk_ctx_init(&ctx, nullptr, nullptr);

    // 1. Fuzz form URL-encoded parsing
    // Set the body data for form parsing
    ctx.request.body = input;
    ctx.request.body_len = size;
    csilk_parse_form_urlencoded(&ctx);

    // 2. Fuzz header operations
    csilk_set_header(&ctx, "X-Fuzz-Header", input);

    // 3. Fuzz query string parsing
    csilk_parse_query(&ctx, input);

    csilk_ctx_cleanup(&ctx);
    if (ctx.arena) {
        csilk_arena_free(ctx.arena);
    }
    free(input);

    return 0;
}
