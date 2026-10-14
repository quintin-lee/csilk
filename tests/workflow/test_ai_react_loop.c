#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai/tool_registry.h"
#include "csilk/drivers/ai.h"
#include "csilk/core/json.h"

/* --- Tool Registry Tests --- */

static char*
mock_add_fn(const char* args_json, void* user_data)
{
    (void)user_data;
    csilk_json_t* obj = csilk_json_parse(args_json);
    if (!obj) {
        return strdup("{\"error\":\"invalid json\"}");
    }
    int64_t a = csilk_json_as_int64(csilk_json_get(obj, "a"), 0);
    int64_t b = csilk_json_as_int64(csilk_json_get(obj, "b"), 0);
    csilk_json_free(obj);

    char res[64];
    snprintf(res, sizeof(res), "{\"result\":%ld}", (long)(a + b));
    return strdup(res);
}

static void
test_tool_registry_features(void)
{
    csilk_ai_tool_registry_t* reg = csilk_ai_tool_registry_new();
    assert(reg != NULL);
    assert(csilk_ai_tool_registry_get_count(reg) == 0);

    const char* schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},\"b\":{\"type\":\""
        "integer\"}},\"required\":[\"a\",\"b\"]}";

    assert(csilk_ai_tool_registry_register(
               reg, "calc_add", "Adds two numbers", schema, mock_add_fn, NULL) == 0);
    assert(csilk_ai_tool_registry_get_count(reg) == 1);

    /* Duplicate registration should fail */
    assert(csilk_ai_tool_registry_register(
               reg, "calc_add", "Duplicate", schema, mock_add_fn, NULL) == -1);

    /* 1. Dispatch with missing required parameter 'b' */
    char* err_res = csilk_ai_tool_dispatch(reg, "calc_add", "{\"a\":5}");
    assert(err_res != NULL);
    assert(strstr(err_res, "missing required parameter") != NULL);
    free(err_res);

    /* 2. Dispatch with valid parameters */
    char* ok_res = csilk_ai_tool_dispatch(reg, "calc_add", "{\"a\":12,\"b\":18}");
    assert(ok_res != NULL);
    assert(strstr(ok_res, "\"result\":30") != NULL);
    free(ok_res);

    /* 3. Export tools array */
    size_t           tool_count = 0;
    csilk_ai_tool_t* tools = csilk_ai_tool_registry_export_tools(reg, &tool_count);
    assert(tools != NULL);
    assert(tool_count == 1);
    assert(strcmp(tools[0].function.name, "calc_add") == 0);
    csilk_ai_tools_free(tools, tool_count);

    /* 4. Unregister */
    assert(csilk_ai_tool_registry_unregister(reg, "calc_add") == 0);
    assert(csilk_ai_tool_registry_get_count(reg) == 0);

    csilk_ai_tool_registry_free(reg);
}

/* --- Mock AI Driver for ReAct Multi-Turn Loop --- */

static int g_mock_chat_turn = 0;

static void*
mock_driver_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    (void)base_url;
    return (void*)1; /* Dummy non-null state */
}

static int
mock_driver_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    g_mock_chat_turn++;

    if (g_mock_chat_turn == 1) {
        /* Turn 1: Model requests tool call 'get_temp' */
        assert(req->message_count >= 1);
        res->content = strdup("Checking the temperature...");
        res->tool_call_count = 1;
        res->tool_calls = calloc(1, sizeof(csilk_ai_tool_call_t));
        res->tool_calls[0].id = strdup("call_temp_001");
        res->tool_calls[0].name = strdup("get_temp");
        res->tool_calls[0].arguments = strdup("{\"city\":\"Berlin\"}");
        res->prompt_tokens = 50;
        res->completion_tokens = 20;
        res->total_tokens = 70;
        return 0;
    }

    if (g_mock_chat_turn == 2) {
        /* Turn 2: Model receives tool result and produces final answer */
        /* Must see the tool message with tool_call_id "call_temp_001" */
        bool saw_tool_msg = false;
        for (size_t i = 0; i < req->message_count; i++) {
            if (strcmp(req->messages[i].role, "tool") == 0 && req->messages[i].tool_call_id &&
                strcmp(req->messages[i].tool_call_id, "call_temp_001") == 0) {
                saw_tool_msg = true;
                break;
            }
        }
        assert(saw_tool_msg == true);

        res->content = strdup("The temperature in Berlin is 18 degrees Celsius.");
        res->tool_call_count = 0;
        res->tool_calls = NULL;
        res->prompt_tokens = 80;
        res->completion_tokens = 15;
        res->total_tokens = 95;
        return 0;
    }

    return -1;
}

static int
mock_driver_embeddings(void*                           state,
                       const char*                     model,
                       const char**                    input,
                       size_t                          count,
                       csilk_ai_embeddings_response_t* res)
{
    (void)state;
    (void)model;
    (void)input;
    (void)count;
    (void)res;
    return 0;
}

static void
mock_driver_free(void* state)
{
    (void)state;
}

static csilk_ai_driver_t s_mock_driver = {
    .name = "mock_react_driver",
    .init = mock_driver_init,
    .chat = mock_driver_chat,
    .embeddings = mock_driver_embeddings,
    .free = mock_driver_free,
};

static char*
mock_get_temp_fn(const char* args_json, void* user_data)
{
    (void)args_json;
    (void)user_data;
    return strdup("{\"temp\":18,\"unit\":\"C\"}");
}

static void
test_react_loop_flow(void)
{
    csilk_ai_register_driver(&s_mock_driver);

    csilk_ai_t* ai = csilk_ai_new("mock_react_driver", "dummy-key", NULL);
    assert(ai != NULL);

    csilk_ai_tool_registry_t* reg = csilk_ai_tool_registry_new();
    assert(reg != NULL);
    assert(csilk_ai_tool_registry_register(
               reg, "get_temp", "Get city temperature", NULL, mock_get_temp_fn, NULL) == 0);

    g_mock_chat_turn = 0;

    char* final_text = NULL;
    int   rc = csilk_ai_react_execute_loop(ai,
                                           "mock-model",
                                           "You are a helpful assistant.",
                                           "What is the temp in Berlin?",
                                           reg,
                                           5,
                                           NULL,
                                           NULL,
                                           &final_text);

    assert(rc == 0);
    assert(final_text != NULL);
    assert(strstr(final_text, "18 degrees Celsius") != NULL);
    assert(g_mock_chat_turn == 2);

    free(final_text);
    csilk_ai_tool_registry_free(reg);
    csilk_ai_free(ai);
}

int
main(void)
{
    test_tool_registry_features();
    test_react_loop_flow();
    printf("All test_ai_react_loop tests passed!\n");
    return 0;
}
