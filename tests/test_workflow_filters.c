#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static char g_final_prompt[1024];
static int  g_done = 0;

/* --- Mock AI Driver to capture prompt --- */
static int
mock_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    // Capture the first user message content
    for (size_t i = 0; i < req->message_count; i++) {
        if (strcmp(req->messages[i].role, "user") == 0) {
            strncpy(g_final_prompt, req->messages[i].content, sizeof(g_final_prompt) - 1);
            break;
        }
    }

    memset(res, 0, sizeof(*res));
    res->content = strdup("ok");
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

csilk_data_t*
producer_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "  hello world  "));
}

void
on_filter_done(csilk_data_t* result)
{
    (void)result;
    g_done = 1;
}

void
test_workflow_template_filters()
{
    printf("Testing template engine filters...\n");
    g_done = 0;
    memset(g_final_prompt, 0, sizeof(g_final_prompt));

    csilk_ai_register_driver(&mock_driver);
    setenv("AGENT_API_KEY", "mock", 1);

    csilk_wf_t* wf = csilk_wf_new("filter_wf");

    csilk_wf_node_t* n1 = csilk_wf_add(wf, "prod", producer_handler, nullptr);

    // AI Node using multiple filters
    csilk_wf_node_t* n2 = csilk_wf_add_ai(
        wf,
        "agent",
        &(csilk_ai_config_t){.prompt = "CMD: {{prod.value | trim | upper | summarize:5}}"});

    csilk_wf_bind(n1, n2);
    csilk_wf_node_set_entry(n1, 1);

    csilk_wf_run(wf, nullptr, on_filter_done);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(g_done == 1);
    /*
   * Original: "  hello world  "
   * trim -> "hello world"
   * upper -> "HELLO WORLD"
   * summarize:5 -> "HELLO"
   */
    printf("[Test] Final Prompt: %s\n", g_final_prompt);
    assert(strcmp(g_final_prompt, "CMD: HELLO") == 0);

    csilk_wf_free(wf);
    printf("test_workflow_template_filters: PASS\n");
}

int
main()
{
    test_workflow_template_filters();
    return 0;
}
