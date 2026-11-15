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
        /* The child exit()s normally (not _exit) so a coverage build's libgcov
         * flushes its .gcda. Under ASAN/LSAN that normal exit would trip the
         * leak check on the stdio server's teardown and make exit non-zero, so
         * disable leak detection in the child only. */
        setenv("ASAN_OPTIONS", "detect_leaks=0", 1);
        setenv("LSAN_OPTIONS", "exitcode=0", 1);
        /* Redirect stdin/stdout for the child's stdio server. */
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
        csilk_mcp_server_register_resource(
            server, "csilk://readme", "Readme", "text/plain", "MCP Docs", NULL, NULL);
        csilk_mcp_server_register_prompt(server, "greeting", "User greeting", "Hello {{name}}");
        int child_rc = csilk_mcp_server_start_stdio(server);
        csilk_mcp_server_free(server);
        /* Return normally (not _exit) so that under a coverage build the
         * child's libgcov atexit handler flushes its .gcda; otherwise the
         * entire handle_mcp_request dispatch (executed in this child) would
         * be invisible to the parent's coverage report. */
        fflush(NULL);
        exit(child_rc == 0 ? 0 : 1);
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

static void
test_ping(void)
{
    printf("Testing MCP ping request...\n");
    const char* input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
    run_stdio_test(input, "result");
    printf("  passed\n");
}

static void
test_resources(void)
{
    printf("Testing MCP resources/list and resources/read...\n");
    const char* list = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/list\"}\n";
    run_stdio_test(list, "csilk://readme");

    const char* read_cmd = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"resources/"
                           "read\",\"params\":{\"uri\":\"csilk://readme\"}}\n";
    run_stdio_test(read_cmd, "MCP Docs");
    printf("  passed\n");
}

static void
test_prompts(void)
{
    printf("Testing MCP prompts/list and prompts/get...\n");
    const char* list = "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"prompts/list\"}\n";
    run_stdio_test(list, "greeting");

    const char* get_cmd = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"prompts/"
                          "get\",\"params\":{\"name\":\"greeting\"}}\n";
    run_stdio_test(get_cmd, "Hello {{name}}");
    printf("  passed\n");
}

int
main(void)
{
    test_initialize();
    test_ping();
    test_tools_list();
    test_tools_call();
    test_resources();
    test_prompts();
    test_invalid_method();
    test_missing_params();
    printf("All test_mcp_stdio tests passed!\n");
    return 0;
}
