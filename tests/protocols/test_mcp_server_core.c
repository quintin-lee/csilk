/**
 * @file test_mcp_server_core.c
 * @brief Unit tests for MCP server core lifecycle (mcp_server.c).
 * @copyright MIT License
 */

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
    return strdup("{\"result\":\"ok\"}");
}

static void
test_mcp_server_new_defaults(void)
{
    printf("Testing csilk_mcp_server_new with defaults (NULL name/version)...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new(NULL, NULL);
    assert(server != NULL);
    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_new_custom(void)
{
    printf("Testing csilk_mcp_server_new with custom name/version...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("my-server", "2.0.0");
    assert(server != NULL);
    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_free_null(void)
{
    printf("Testing csilk_mcp_server_free with NULL...\n");
    csilk_mcp_server_free(NULL);
    printf("  passed\n");
}

static void
test_mcp_server_register_tool_null_server(void)
{
    printf("Testing csilk_mcp_server_register_tool with NULL server...\n");
    csilk_wf_tool_entry_t tool = {
        .name = "test",
        .description = "desc",
        .parameters_json = "{}",
        .fn = mock_tool_fn,
        .user_data = NULL,
    };
    int res = csilk_mcp_server_register_tool(NULL, &tool);
    assert(res == -1);
    printf("  passed\n");
}

static void
test_mcp_server_register_tool_null_tool(void)
{
    printf("Testing csilk_mcp_server_register_tool with NULL tool...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);
    int res = csilk_mcp_server_register_tool(server, NULL);
    assert(res == -1);
    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_register_tool_null_name(void)
{
    printf("Testing csilk_mcp_server_register_tool with NULL tool name...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);
    csilk_wf_tool_entry_t tool = {
        .name = NULL,
        .description = "desc",
        .parameters_json = "{}",
        .fn = mock_tool_fn,
        .user_data = NULL,
    };
    int res = csilk_mcp_server_register_tool(server, &tool);
    assert(res == -1);
    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_register_multiple_tools(void)
{
    printf("Testing csilk_mcp_server_register_tool multiple tools...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);

    csilk_wf_tool_entry_t tool1 = {
        .name = "tool_one",
        .description = "First tool",
        .parameters_json = "{\"type\":\"object\"}",
        .fn = mock_tool_fn,
        .user_data = NULL,
    };
    csilk_wf_tool_entry_t tool2 = {
        .name = "tool_two",
        .description = "Second tool",
        .parameters_json = "{\"type\":\"object\"}",
        .fn = mock_tool_fn,
        .user_data = NULL,
    };

    int res1 = csilk_mcp_server_register_tool(server, &tool1);
    assert(res1 == 0);
    int res2 = csilk_mcp_server_register_tool(server, &tool2);
    assert(res2 == 0);

    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_register_tool_with_user_data(void)
{
    printf("Testing csilk_mcp_server_register_tool with user_data...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);

    int                   userdata = 42;
    csilk_wf_tool_entry_t tool = {
        .name = "data_tool",
        .description = "Tool with data",
        .parameters_json = "{}",
        .fn = mock_tool_fn,
        .user_data = &userdata,
    };
    int res = csilk_mcp_server_register_tool(server, &tool);
    assert(res == 0);

    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_register_workflow_null(void)
{
    printf("Testing csilk_mcp_server_register_workflow null safety...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);

    int res = csilk_mcp_server_register_workflow(server, NULL);
    assert(res == -1);

    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_register_wasm_tool_null(void)
{
    printf("Testing csilk_mcp_server_register_wasm_tool null safety...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);

    /* Check signature - needs correct number of args */
    int res = csilk_mcp_server_register_wasm_tool(server, NULL, NULL, NULL);
    assert(res == -1);

    csilk_mcp_server_free(server);
    printf("  passed\n");
}

static void
test_mcp_server_bind_app_null(void)
{
    printf("Testing csilk_mcp_server_bind_app null safety...\n");
    csilk_mcp_server_t* server = csilk_mcp_server_new("test", "1.0");
    assert(server != NULL);

    int res = csilk_mcp_server_bind_app(server, NULL, "/mcp");
    assert(res == -1);

    csilk_mcp_server_free(server);
    printf("  passed\n");
}

int
main(void)
{
    test_mcp_server_new_defaults();
    test_mcp_server_new_custom();
    test_mcp_server_free_null();
    test_mcp_server_register_tool_null_server();
    test_mcp_server_register_tool_null_tool();
    test_mcp_server_register_tool_null_name();
    test_mcp_server_register_multiple_tools();
    test_mcp_server_register_tool_with_user_data();
    test_mcp_server_register_workflow_null();
    test_mcp_server_register_wasm_tool_null();
    test_mcp_server_bind_app_null();

    printf("All test_mcp_server_core tests passed successfully!\n");
    return 0;
}
