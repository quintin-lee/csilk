#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/drivers/ai.h"

int
main()
{
    printf("Testing csilk_ai_chat_response_free...\n");
    {
        csilk_ai_chat_response_free(nullptr);

        csilk_ai_chat_response_t res = {0};
        res.content = strdup("hello");
        res.raw_response = strdup("{\"ok\": true}");
        res.error_message = nullptr;
        csilk_ai_chat_response_free(&res);
    }

    {
        csilk_ai_chat_response_t res = {0};
        res.error_message = strdup("error occurred");
        res.content = nullptr;
        res.raw_response = nullptr;
        csilk_ai_chat_response_free(&res);
    }

    printf("Testing csilk_ai_embeddings_response_free...\n");
    {
        csilk_ai_embeddings_response_free(nullptr);

        csilk_ai_embeddings_response_t res = {0};
        res.values = malloc(10 * sizeof(float));
        res.error_message = nullptr;
        csilk_ai_embeddings_response_free(&res);
    }

    {
        csilk_ai_embeddings_response_t res = {0};
        res.values = nullptr;
        res.error_message = strdup("error");
        csilk_ai_embeddings_response_free(&res);
    }

    printf("Testing csilk_ai_context_new/free...\n");
    {
        csilk_ai_context_t* ctx = csilk_ai_context_new(0);
        assert(ctx != nullptr);
        csilk_ai_context_add(ctx, "user", "Hello");
        assert(ctx->count == 1);
        csilk_ai_context_free(ctx);
    }

    printf("Testing csilk_ai_context sliding window...\n");
    {
        csilk_ai_context_t* ctx = csilk_ai_context_new(2);
        assert(ctx != nullptr);
        csilk_ai_context_add(ctx, "user", "msg1");
        csilk_ai_context_add(ctx, "assistant", "msg2");
        csilk_ai_context_add(ctx, "user", "msg3");
        assert(ctx->count == 2);
        assert(strcmp(ctx->messages[0].role, "assistant") == 0);
        assert(strcmp(ctx->messages[1].content, "msg3") == 0);
        csilk_ai_context_free(ctx);
    }

    printf("Testing csilk_ai_context nullptr safety...\n");
    {
        csilk_ai_context_t* ctx2 = csilk_ai_context_new(10);
        csilk_ai_context_add(nullptr, "user", "hi");
        csilk_ai_context_add(ctx2, nullptr, "hi");
        csilk_ai_context_add(ctx2, "user", nullptr);
        csilk_ai_context_clear(nullptr);
        csilk_ai_context_free(nullptr);
        csilk_ai_context_free(ctx2);
    }

    printf("Testing csilk_ai_context_clear...\n");
    {
        csilk_ai_context_t* ctx = csilk_ai_context_new(10);
        assert(ctx != nullptr);
        csilk_ai_context_add(ctx, "user", "data");
        csilk_ai_context_clear(ctx);
        assert(ctx->count == 0);
        csilk_ai_context_free(ctx);
    }

    /* --- Tool calls serialization test --- */
    printf("Testing csilk_ai_message_t tool_calls serialization...\n");
    {
        csilk_ai_tool_call_t tc = {.id = strdup("call_test001"),
                                   .name = strdup("get_weather"),
                                   .arguments = strdup("{\"city\":\"Shanghai\"}")};
        csilk_ai_message_t   msg = {
            .role = "assistant",
            .content = NULL,
            .tool_call_count = 1,
            .tool_calls = &tc,
            .tool_call_id = NULL,
        };

        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_string(obj, "role", msg.role);
        csilk_json_add_string(obj, "content", msg.content ? msg.content : "");
        csilk_json_t* tc_arr = csilk_json_array();
        for (size_t j = 0; j < msg.tool_call_count; j++) {
            const csilk_ai_tool_call_t* t = &msg.tool_calls[j];
            csilk_json_t*               tc_obj = csilk_json_object();
            csilk_json_add_string(tc_obj, "id", t->id);
            csilk_json_add_string(tc_obj, "type", "function");
            csilk_json_t* fn = csilk_json_object();
            csilk_json_add_string(fn, "name", t->name);
            csilk_json_add_string(fn, "arguments", t->arguments);
            csilk_json_add_object(tc_obj, "function", fn);
            csilk_json_array_append(tc_arr, tc_obj);
        }
        csilk_json_add_array(obj, "tool_calls", tc_arr);

        size_t len = 0;
        char*  json_str = csilk_json_serialize(obj, &len);
        csilk_json_free(obj);

        assert(json_str != NULL);
        assert(strstr(json_str, "\"id\":\"call_test001\"") != NULL);
        assert(strstr(json_str, "\"name\":\"get_weather\"") != NULL);
        assert(strstr(json_str, "\"content\":\"{\\\"tool_calls\"") == NULL);
        printf("  assistant+tool_calls JSON OK: %s\n", json_str);
        free(json_str);
        free(tc.id);
        free(tc.name);
        free(tc.arguments);
    }

    /* --- Tool result message test --- */
    printf("Testing csilk_ai_message_t tool result serialization...\n");
    {
        csilk_ai_message_t msg = {
            .role = "tool",
            .content = "ok",
            .tool_call_id = strdup("call_test001"),
            .tool_calls = NULL,
            .tool_call_count = 0,
        };

        csilk_json_t* obj = csilk_json_object();
        csilk_json_add_string(obj, "role", msg.role);
        csilk_json_add_string(obj, "tool_call_id", msg.tool_call_id);
        csilk_json_add_string(obj, "content", msg.content);

        size_t len = 0;
        char*  json_str = csilk_json_serialize(obj, &len);
        csilk_json_free(obj);

        assert(json_str != NULL);
        assert(strstr(json_str, "\"tool_call_id\":\"call_test001\"") != NULL);
        assert(strstr(json_str, "\"content\":\"ok\"") != NULL);
        printf("  tool result JSON OK: %s\n", json_str);
        free(json_str);
        free((void*)msg.tool_call_id);
    }

    /* --- Context add_tool_result test --- */
    printf("Testing csilk_ai_context_add_tool_result...\n");
    {
        csilk_ai_context_t* ctx = csilk_ai_context_new(0);
        assert(ctx != NULL);

        csilk_ai_tool_call_t     tca = {.id = strdup("call_ctx01"),
                                        .name = strdup("calc"),
                                        .arguments = strdup("{\"a\":1,\"b\":2}")};
        csilk_ai_chat_response_t res = {
            .content = NULL,
            .tool_call_count = 1,
            .tool_calls = &tca,
        };

        csilk_ai_context_add_tool_result(ctx, &res);
        assert(ctx->count == 2);
        assert(strcmp(ctx->messages[0].role, "assistant") == 0);
        assert(ctx->messages[0].tool_call_count == 1);
        assert(strcmp(ctx->messages[0].tool_calls[0].id, "call_ctx01") == 0);
        assert(strcmp(ctx->messages[1].role, "tool") == 0);
        assert(strcmp(ctx->messages[1].tool_call_id, "call_ctx01") == 0);

        csilk_ai_context_free(ctx);
        free(tca.id);
        free(tca.name);
        free(tca.arguments);
    }

    /* --- Cleanup test: clear sets pointers to NULL safely --- */
    printf("Testing csilk_ai_context_clear frees tool_calls correctly...\n");
    {
        csilk_ai_context_t* ctx = csilk_ai_context_new(10);
        assert(ctx != NULL);
        csilk_ai_context_add(ctx, "user", "hi");

        csilk_ai_tool_call_t tcc = {
            .id = strdup("call_clean"), .name = strdup("fn"), .arguments = strdup("{}")};
        csilk_ai_chat_response_t res = {
            .content = NULL,
            .tool_call_count = 1,
            .tool_calls = &tcc,
        };
        csilk_ai_context_add_tool_result(ctx, &res);
        assert(ctx->count == 3);

        csilk_ai_context_clear(ctx);
        assert(ctx->count == 0);
        csilk_ai_context_free(ctx);
        free(tcc.id);
        free(tcc.name);
        free(tcc.arguments);
    }

    printf("test_ai_ext: PASS\n");
    return 0;
}
