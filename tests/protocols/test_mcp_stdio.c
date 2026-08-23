#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "csilk/protocols/mcp.h"

static char*
mock_tool_fn(const char* args_json, void* user_data)
{
    (void)args_json;
    (void)user_data;
    return strdup("{\"result\":\"ok\"}");
}

static void
run_stdio_test(const char* input, const char* expected)
{
    int pipe_in[2], pipe_out[2];
    pipe(pipe_in);
    pipe(pipe_out);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: redirect stdin/stdout */
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);

        csilk_mcp_server_t* server = csilk_mcp_server_new("test-srv", "1.0.0");
        assert(server != NULL);
        csilk_wf_tool_entry_t tool = {
            .name = "echo",
            .description = "echo tool",
            .parameters_json = "{\"type\":\"object\"}",
            .fn = mock_tool_fn,
            .user_data = NULL,
        };
        csilk_mcp_server_register_tool(server, &tool);
        csilk_mcp_server_start_stdio(server);
        csilk_mcp_server_free(server);
        _exit(0);
    }

    /* Parent: write input, read response */
    close(pipe_in[0]);
    close(pipe_out[1]);
    write(pipe_in[1], input, strlen(input));
    close(pipe_in[1]);

    char buf[4096] = {0};
    read(pipe_out[0], buf, sizeof(buf) - 1);
    close(pipe_out[0]);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(strstr(buf, expected) != NULL);
}

static void
test_initialize(void)
{
    printf("Testing MCP initialize request...\n");
    const char* input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n";
    run_stdio_test(input, "protocolVersion");
    printf("  passed\n");
}

static void
test_tools_list(void)
{
    printf("Testing MCP tools/list request...\n");
    const char* init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n";
    run_stdio_test(init, "protocolVersion");
    const char* list = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    run_stdio_test(list, "echo");
    printf("  passed\n");
}

static void
test_tools_call(void)
{
    printf("Testing MCP tools/call request...\n");
    const char* init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n";
    run_stdio_test(init, "protocolVersion");
    const char* call = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/"
                       "call\",\"params\":{\"name\":\"echo\",\"arguments\":{}}}\n";
    run_stdio_test(call, "ok");
    printf("  passed\n");
}

static void
test_invalid_method(void)
{
    printf("Testing MCP unknown method...\n");
    const char* input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"unknown\"}\n";
    run_stdio_test(input, "Method not implemented");
    printf("  passed\n");
}

static void
test_missing_params(void)
{
    printf("Testing MCP tools/call missing params...\n");
    const char* input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\"}\n";
    run_stdio_test(input, "Missing params");
    printf("  passed\n");
}

int
main(void)
{
    test_initialize();
    test_tools_list();
    test_tools_call();
    test_invalid_method();
    test_missing_params();
    printf("All test_mcp_stdio tests passed!\n");
    return 0;
}
