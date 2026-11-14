/**
 * @file test_mcp_sse_ext.c
 * @brief Unit tests for MCP Server & Client SSE (Server-Sent Events) transport
 * and boundary edge cases.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/protocols/mcp.h"
#include "csilk/test/test.h"

static void
test_mcp_sse_stream_boundary(void)
{
    printf("Testing MCP SSE stream boundary and malformed inputs...");

    /* 1. Verify standard csilk_mcp_server_new with NULL name */
    csilk_mcp_server_t* s = csilk_mcp_server_new(NULL, NULL);
    assert(s != NULL);
    csilk_mcp_server_free(s);

    /* 2. Verify csilk_mcp_server_new with extremely long names (OOM edge cases) */
    char long_name[1024];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    csilk_mcp_server_t* s2 = csilk_mcp_server_new(long_name, "extreme-length");
    assert(s2 != NULL);
    csilk_mcp_server_free(s2);

    /* 3. Verify MCP Client connect with NULL or empty URL */
    csilk_mcp_client_t* c_null = csilk_mcp_client_connect_sse(NULL);
    assert(c_null == NULL);

    csilk_mcp_client_t* c_empty = csilk_mcp_client_connect_sse("");
    assert(c_empty != NULL);
    csilk_mcp_client_free(c_empty);

    printf(" passed\n");
}

int
main(void)
{
    test_mcp_sse_stream_boundary();
    printf("test_mcp_sse_ext: ALL PASSED\n");
    return 0;
}
