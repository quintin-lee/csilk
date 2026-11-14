#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "protocols/mcp/mcp_internal.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) {
        return 0;
    }

    /* 1. Exercise heap-allocated JSON-RPC parse and serialize */
    csilk_mcp_msg_t* msg = csilk_mcp_msg_parse((const char*)data, size, nullptr);
    if (msg) {
        char* out = csilk_mcp_msg_serialize(msg, nullptr);
        free(out);

        /* Build and serialize error frame echoing the parsed id */
        csilk_mcp_msg_t* err =
            csilk_mcp_msg_create_error(msg->id, CSILK_MCP_INVALID_PARAMS, "Fuzz error");
        if (err) {
            char* err_out = csilk_mcp_msg_serialize(err, nullptr);
            free(err_out);
            csilk_mcp_msg_destroy(err, nullptr);
        }

        csilk_mcp_msg_destroy(msg, nullptr);
    }

    /* 2. Exercise arena-allocated JSON-RPC parse and serialize */
    csilk_arena_t* arena = csilk_arena_new(32768);
    if (arena) {
        csilk_mcp_msg_t* a_msg = csilk_mcp_msg_parse((const char*)data, size, arena);
        if (a_msg) {
            char* a_out = csilk_mcp_msg_serialize(a_msg, arena);
            (void)a_out;
            csilk_mcp_msg_destroy(a_msg, arena);
        }
        csilk_arena_free(arena);
    }

    return 0;
}
