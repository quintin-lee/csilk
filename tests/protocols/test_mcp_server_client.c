#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/protocols/mcp.h"

static char*
mock_tool_fn(const char* args_json, void* user_data)
{
    (void)args_json;
    (void)user_data;
    return strdup("{\"result\":\"success\"}");
}

static void
test_mcp_server_creation_and_registration(void)
{
    csilk_mcp_server_t* server = csilk_mcp_server_new("test-server", "1.0.0");
    assert(server != nullptr);

    csilk_wf_tool_entry_t tool = {
        .name = "calculator",
        .description = "Basic calculator tool",
        .parameters_json = "{\"type\":\"object\"}",
        .fn = mock_tool_fn,
        .user_data = nullptr,
    };

    int res = csilk_mcp_server_register_tool(server, &tool);
    assert(res == 0);

    csilk_mcp_server_free(server);
    printf("test_mcp_server_creation_and_registration passed\n");
}

static void
test_mcp_client_import(void)
{
    csilk_mcp_client_t* client = csilk_mcp_client_connect_sse("http://localhost:8080/mcp/sse");
    assert(client != nullptr);

    csilk_mcp_client_free(client);
    printf("test_mcp_client_import passed\n");
}

int
main(void)
{
    test_mcp_server_creation_and_registration();
    test_mcp_client_import();
    printf("All test_mcp_server_client tests passed!\n");
    return 0;
}
