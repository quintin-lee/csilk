#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/ctx/ctx_internal.h"

static void
_fuzz_ws_on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
    (void)c;
    (void)payload;
    (void)len;
    (void)opcode;
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) {
        return 0;
    }

    csilk_ctx_t ctx;
    _csilk_ctx_init(&ctx, nullptr, nullptr);
    csilk_set_on_ws_message(&ctx, _fuzz_ws_on_message);

    /* Exercise WebSocket upgrade handshake with fuzzed Sec-WebSocket-Key */
    size_t key_len = size < 255 ? size : 255;
    char   key_buf[256];
    memcpy(key_buf, data, key_len);
    key_buf[key_len] = '\0';
    csilk_set_request_header(&ctx, "Sec-WebSocket-Key", key_buf);
    csilk_ws_handshake(&ctx);

    /* Feed raw WebSocket frames in two chunks to exercise incremental buffering */
    size_t split = size / 2;
    if (split > 0) {
        csilk_ws_parse_frame(&ctx, data, split);
        csilk_ws_parse_frame(&ctx, data + split, size - split);
    } else {
        csilk_ws_parse_frame(&ctx, data, size);
    }

    csilk_ctx_cleanup(&ctx);
    if (ctx.arena) {
        csilk_arena_free(ctx.arena);
    }
    return 0;
}
