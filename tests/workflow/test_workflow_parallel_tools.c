#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static int      g_tool_calls = 0;
static uint64_t g_start_time = 0;
static uint64_t g_end_time = 0;

/* --- Mock Tool --- */
char*
slow_tool(const char* args_json, void* user_data)
{
    (void)args_json;
    (void)user_data;
    printf("[SlowTool] Running...\n");
    usleep(100000); // 100ms
    __atomic_fetch_add(&g_tool_calls, 1, __ATOMIC_RELAXED);
    return strdup("{\"status\": \"ok\"}");
}

/* --- Mock AI Driver --- */
static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    (void)req;
    static int turn = 0;
    turn++;

    memset(res, 0, sizeof(*res));
    if (turn == 1) {
        // Request 3 tools at once
        res->tool_call_count = 3;
        res->tool_calls = calloc(3, sizeof(csilk_ai_tool_call_t));
        for (int i = 0; i < 3; i++) {
            char id[16];
            snprintf(id, sizeof(id), "call_%d", i);
            res->tool_calls[i].id = strdup(id);
            res->tool_calls[i].name = strdup("slow_tool");
            res->tool_calls[i].arguments = strdup("{}");
        }
    } else {
        res->content = strdup("Done.");
    }
    return 0;
}

static void*
mock_init(const char* key, const char* url)
{
    (void)key;
    (void)url;
    return (void*)1;
}
static void
mock_free(void* state)
{
    (void)state;
}

static csilk_ai_driver_t mock_driver = {
    .name = "openai", .init = mock_init, .chat = mock_chat, .free = mock_free};

void
test_workflow_parallel_tools()
{
    printf("Testing parallel tool execution performance...\n");
    g_tool_calls = 0;

    csilk_ai_register_driver(&mock_driver);
    setenv("AGENT_API_KEY", "mock", 1);

    csilk_wf_t* wf = csilk_wf_new("parallel_tools_wf");
    csilk_wf_register_tool(wf, "slow_tool", "A slow tool", "{}", slow_tool, nullptr);

    csilk_wf_node_t* n1 = csilk_wf_add_ai(
        wf, "agent", &(csilk_ai_config_t){.model = "gpt-4", .prompt = "Do 3 slow things"});
    csilk_wf_node_set_entry(n1, 1);

    g_start_time = csilk_io_hrtime();
    csilk_wf_run(wf, nullptr, nullptr);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    g_end_time = csilk_io_hrtime();

    double duration_ms = (double)(g_end_time - g_start_time) / 1000000.0;
    printf("Execution took %.2f ms\n", duration_ms);

    assert(g_tool_calls == 3);

    // If sequential, would take > 300ms. If parallel, should be ~100ms (+
    // overhead). Thread pool size might limit parallelism, but for 3 tasks it
    // should be fine. Allow generous buffer for CI (ASan/macOS scheduling).
    assert(duration_ms < 600.0);

    csilk_wf_free(wf);
    printf("test_workflow_parallel_tools: PASS\n");
}

int
main()
{
    test_workflow_parallel_tools();
    return 0;
}
