#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/drivers/ai.h"
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

/* NULL / uninitialized-client boundary guards in the MCP client layer.
 * connect_sse returns a handle even when the endpoint is unreachable, so we
 * can exercise the "not initialized" failure paths without a live server. */
static void
test_mcp_client_boundary_guards(void)
{
    /* NULL-client / NULL-tool-name guards. */
    assert(csilk_mcp_client_call_tool(nullptr, "tool", "{}") == nullptr);
    assert(csilk_mcp_client_call_tool(nullptr, nullptr, "{}") == nullptr);
    assert(csilk_mcp_client_import_to_registry(nullptr, nullptr) != 0);
    csilk_mcp_client_free(nullptr); /* safe no-op */

    /* A client whose initialize round-trip cannot complete (no live server)
     * must have call_tool fail cleanly and return NULL. */
    csilk_mcp_client_t* dead = csilk_mcp_client_connect_sse("http://127.0.0.1:1/mcp/sse");
    assert(dead != nullptr);
    char* out = csilk_mcp_client_call_tool(dead, "ping", "{}");
    assert(out == nullptr);
    csilk_mcp_client_free(dead);
    printf("test_mcp_client_boundary_guards passed\n");
}

static void
test_mcp_ws_transport(void)
{
    /* Test server-side WebSocket binding */
    csilk_mcp_server_t* srv = csilk_mcp_server_new("test-ws-server", "1.0.0");
    assert(srv != nullptr);

    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != nullptr);

    assert(csilk_mcp_server_bind_ws(srv, app, "/mcp/ws") == 0);
    assert(csilk_mcp_server_bind_ws(nullptr, app, "/mcp/ws") == -1);
    assert(csilk_mcp_server_bind_ws(srv, nullptr, "/mcp/ws") == -1);

    /* Test bind_app auto-binds /ws */
    assert(csilk_mcp_server_bind_app(srv, app, "/mcp2") == 0);

    /* Test client-side WebSocket handle creation */
    assert(csilk_mcp_client_connect_ws(nullptr) == nullptr);
    csilk_mcp_client_t* ws_client = csilk_mcp_client_connect_ws("ws://127.0.0.1:8080/mcp/ws");
    assert(ws_client != nullptr);
    csilk_mcp_client_free(ws_client);

    csilk_app_free(app);
    csilk_mcp_server_free(srv);
    printf("test_mcp_ws_transport passed\n");
}

static void
test_mcp_client_context_injection(void)
{
    /* NULL safety */
    assert(csilk_mcp_client_inject_context(nullptr, nullptr) == -1);

    /* Mock stdio server that serves initialize, prompts/list, and resources/list */
    const char* script = "/tmp/csilk_mock_mcp_ctx.sh";
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
            "    *'\"method\":\"prompts/list\"'*)\n"
            "      printf "
            "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"prompts\":[{\"name\":\"code_review\","
            "\"description\":\"Audit C23 code for safety\"}]}}\\n'\n"
            "      ;;\n"
            "    *'\"method\":\"resources/list\"'*)\n"
            "      printf "
            "'{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"resources\":[{\"uri\":\"csilk://schema\","
            "\"name\":\"DB Schema\"}]}}\\n'\n"
            "      ;;\n"
            "  esac\n"
            "done\n");
    fclose(f);

    char*               argv[] = {"/bin/sh", (char*)script, nullptr};
    csilk_mcp_client_t* client = csilk_mcp_client_connect_stdio("/bin/sh", argv);
    assert(client != nullptr);

    csilk_ai_context_t* ctx = csilk_ai_context_new(10);
    assert(ctx != nullptr);

    int injected = csilk_mcp_client_inject_context(client, ctx);
    assert(injected == 2);
    assert(ctx->count == 2);
    assert(ctx->messages[0].content != nullptr);
    assert(strstr(ctx->messages[0].content, "code_review") != nullptr);
    assert(ctx->messages[1].content != nullptr);
    assert(strstr(ctx->messages[1].content, "csilk://schema") != nullptr);

    csilk_ai_context_free(ctx);
    csilk_mcp_client_free(client);
    remove(script);
    printf("test_mcp_client_context_injection passed\n");
}

int
main(void)
{
    test_mcp_server_creation_and_registration();
    test_mcp_client_import();
    test_mcp_client_bridge_to_registry();
    test_mcp_client_boundary_guards();
    test_mcp_ws_transport();
    test_mcp_client_context_injection();
    printf("All test_mcp_server_client tests passed!\n");
    return 0;
}
