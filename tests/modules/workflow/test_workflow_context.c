#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static int g_max_msgs_seen = 0;
static int g_system_msg_preserved = 0;
static int g_done = 0;

/* --- Mock AI Driver for Truncation --- */
static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    static int turns = 0;
    turns++;

    if ((int)req->message_count > g_max_msgs_seen) {
        g_max_msgs_seen = (int)req->message_count;
    }

    if (req->message_count > 0 && strcmp(req->messages[0].role, "system") == 0) {
        if (strcmp(req->messages[0].content, "Instructions") == 0) {
            g_system_msg_preserved = 1;
        }
    }

    printf("[MockAI] Turn %d, Message Count: %zu\n", turns, req->message_count);

    memset(res, 0, sizeof(*res));
    if (turns < 8) {
        // Force another turn with a dummy tool call
        res->content = strdup("Thinking...");
        res->tool_call_count = 1;
        res->tool_calls = calloc(1, sizeof(csilk_ai_tool_call_t));
        res->tool_calls[0].id = strdup("call_1");
        res->tool_calls[0].name = strdup("dummy");
        res->tool_calls[0].arguments = strdup("{}");
    } else {
        res->content = strdup("Final Answer");
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
on_context_done(csilk_data_t* result)
{
    (void)result;
    g_done = 1;
}

void
test_workflow_context_truncation()
{
    printf("Testing Agent context window truncation...\n");
    g_max_msgs_seen = 0;
    g_system_msg_preserved = 0;
    g_done = 0;

    csilk_ai_register_driver(&mock_driver);
    setenv("AGENT_API_KEY", "mock", 1);

    csilk_wf_t* wf = csilk_wf_new("context_wf");

    // Node with strict history limit
    csilk_wf_node_t* n1 = csilk_wf_add_ai(wf,
                                          "agent",
                                          &(csilk_ai_config_t){.system_msg = "Instructions",
                                                               .prompt = "Start long conversation",
                                                               .max_history_messages = 4});
    csilk_wf_node_set_entry(n1, 1);

    csilk_wf_run(wf, nullptr, on_context_done);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(g_done == 1);
    // The message count should never have exceeded 4
    assert(g_max_msgs_seen <= 4);
    // The system message should have been kept
    assert(g_system_msg_preserved == 1);

    csilk_wf_free(wf);
    printf("test_workflow_context_truncation: PASS\n");
}

int
main()
{
    test_workflow_context_truncation();
    return 0;
}
