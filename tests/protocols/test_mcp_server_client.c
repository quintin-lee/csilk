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

static void
test_mcp_client_bridge_to_registry(void)
{
    /* Write a tiny POSIX-sh mock MCP stdio server that speaks initialize,
     * tools/list, and tools/call so we verify the full end-to-end bridge from
     * MCP tools/list -> csilk_ai_tool_registry_t -> csilk_ai_tool_dispatch. */
    const char* script = "/tmp/csilk_mock_mcp_srv.sh";
    FILE*       f = fopen(script, "w");
    assert(f != nullptr);
    fprintf(f,
            "#!/bin/sh\n"
            "while IFS= read -r line; do\n"
            "  case \"$line\" in\n"
            "    *'\"method\":\"initialize\"'*)\n"
            "      printf "
            "'{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}\\n'\n"
            "      ;;\n"
            "    *'\"method\":\"tools/list\"'*)\n"
            "      printf "
            "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"calc_sum\","
            "\"description\":\"Add two "
            "integers\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":"
            "\"integer\"}}}}]}}\\n'\n"
            "      ;;\n"
            "    *'\"method\":\"tools/call\"'*)\n"
            "      printf "
            "'{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
            "\"{\\\\\"sum\\\\\":7}\"}],\"isError\":false}}\\n'\n"
            "      ;;\n"
            "  esac\n"
            "done\n");
    fclose(f);

    char*               argv[] = {"/bin/sh", (char*)script, nullptr};
    csilk_mcp_client_t* client = csilk_mcp_client_connect_stdio("/bin/sh", argv);
    assert(client != nullptr);

    csilk_ai_tool_registry_t* reg = csilk_ai_tool_registry_new();
    assert(reg != nullptr);

    int imported = csilk_mcp_client_import_to_registry(client, reg);
    assert(imported == 1);
    assert(csilk_ai_tool_registry_get_count(reg) == 1);

    /* Export schema to verify inputSchema was bridged accurately. */
    size_t           tcnt = 0;
    csilk_ai_tool_t* tools = csilk_ai_tool_registry_export_tools(reg, &tcnt);
    assert(tools != nullptr && tcnt == 1);
    assert(strcmp(tools[0].function.name, "calc_sum") == 0);
    assert(strstr(tools[0].function.description, "Add two integers") != nullptr);
    csilk_ai_tools_free(tools, tcnt);

    /* Dispatch through tool_registry -> remote MCP tools/call over stdio. */
    char* res = csilk_ai_tool_dispatch(reg, "calc_sum", "{\"a\":3}");
    assert(res != nullptr);
    assert(strstr(res, "7") != nullptr);
    free(res);

    /* Also verify workflow import bridges the real discovered tool. */
    csilk_wf_t* wf = csilk_wf_new("mcp-import-wf");
    assert(wf != nullptr);
    assert(csilk_mcp_client_import_tools(client, wf) == 1);
    csilk_wf_free(wf);

    csilk_ai_tool_registry_free(reg);
    csilk_mcp_client_free(client);
    remove(script);
    printf("test_mcp_client_bridge_to_registry passed\n");
}

int
main(void)
{
    test_mcp_server_creation_and_registration();
    test_mcp_client_import();
    test_mcp_client_bridge_to_registry();
    printf("All test_mcp_server_client tests passed!\n");
    return 0;
}
