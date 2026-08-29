/**
 * @file test_context_sizeof.c
 * @brief Memory audit tool printing exact sizes and memory reduction statistics.
 */

#include <stdio.h>
#include <stddef.h>
#include "csilk/csilk.h"
#include "core/ctx/ctx_internal.h"

int
main(void)
{
    printf("============================================================\n");
    printf("CSILK CONTEXT MEMORY AUDIT\n");
    printf("============================================================\n");
    printf("sizeof(csilk_header_t)       = %4zu bytes\n", sizeof(csilk_header_t));
    printf("sizeof(csilk_header_map_t)   = %4zu bytes (16 known slots + 16 buckets + count/used)\n",
           sizeof(csilk_header_map_t));
    printf("sizeof(csilk_kv_map_t)       = %4zu bytes (16 buckets + used)\n",
           sizeof(csilk_kv_map_t));
    printf("sizeof(csilk_request_t)      = %4zu bytes\n", sizeof(csilk_request_t));
    printf("sizeof(csilk_response_t)     = %4zu bytes\n", sizeof(csilk_response_t));
    printf("sizeof(csilk_request_scope_t)= %4zu bytes\n", sizeof(csilk_request_scope_t));
    printf("sizeof(csilk_conn_scope_t)   = %4zu bytes\n", sizeof(csilk_conn_scope_t));
    printf("sizeof(csilk_stream_scope_t) = %4zu bytes\n", sizeof(csilk_stream_scope_t));
    printf("sizeof(csilk_ctx_t)          = %4zu bytes\n", sizeof(csilk_ctx_t));
    printf("============================================================\n");

    size_t ctx_sz = sizeof(csilk_ctx_t);
    printf("Scale Memory Footprint (Contexts only):\n");
    printf("  -   100 concurrent streams: %7.2f KB\n", (double)(100 * ctx_sz) / 1024.0);
    printf("  - 1,000 concurrent streams: %7.2f MB\n", (double)(1000 * ctx_sz) / (1024.0 * 1024.0));
    printf("  -10,000 concurrent streams: %7.2f MB\n",
           (double)(10000 * ctx_sz) / (1024.0 * 1024.0));
    printf("============================================================\n");
    return 0;
}
