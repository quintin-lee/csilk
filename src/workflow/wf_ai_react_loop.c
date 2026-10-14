/**
 * @file wf_ai_react_loop.c
 * @brief Native Tool-Calling ReAct agent execution loop.
 *
 * Drives autonomous Thought-Action-Observation multi-turn loops
 * using OpenAI-compatible function calling specifications.
 *
 * @copyright MIT License
 */

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai/tool_registry.h"
#include "csilk/drivers/ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
free_conversation_messages(csilk_ai_message_t* msgs, size_t count)
{
    if (!msgs) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free((void*)msgs[i].content);
        free((void*)msgs[i].tool_call_id);
        if (msgs[i].tool_calls) {
            for (size_t j = 0; j < msgs[i].tool_call_count; j++) {
                free(msgs[i].tool_calls[j].id);
                free(msgs[i].tool_calls[j].name);
                free(msgs[i].tool_calls[j].arguments);
            }
            free(msgs[i].tool_calls);
        }
    }
    free(msgs);
}

int
csilk_ai_react_execute_loop(csilk_ai_t*               ai,
                            const char*               model,
                            const char*               system_prompt,
                            const char*               user_prompt,
                            csilk_ai_tool_registry_t* reg,
                            int                       max_iterations,
                            csilk_ai_stream_cb        on_chunk,
                            void*                     user_data,
                            char**                    out_final_text)
{
    if (!ai || !user_prompt || !out_final_text) {
        return -1;
    }

    *out_final_text = NULL;
    int max_turns = max_iterations > 0 ? max_iterations : 10;

    size_t           tool_count = 0;
    csilk_ai_tool_t* tools = reg ? csilk_ai_tool_registry_export_tools(reg, &tool_count) : NULL;

    size_t              msg_capacity = 32;
    size_t              msg_count = 0;
    csilk_ai_message_t* msgs = calloc(msg_capacity, sizeof(csilk_ai_message_t));
    if (!msgs) {
        if (tools) {
            csilk_ai_tools_free(tools, tool_count);
        }
        return -1;
    }

    /* 1. Add optional system message */
    if (system_prompt && system_prompt[0]) {
        msgs[msg_count++] = (csilk_ai_message_t){
            .role = "system",
            .content = strdup(system_prompt),
            .tool_calls = NULL,
            .tool_call_count = 0,
            .tool_call_id = NULL,
        };
    }

    /* 2. Add user prompt */
    msgs[msg_count++] = (csilk_ai_message_t){
        .role = "user",
        .content = strdup(user_prompt),
        .tool_calls = NULL,
        .tool_call_count = 0,
        .tool_call_id = NULL,
    };

    int rc = -1;

    for (int turn = 0; turn < max_turns; turn++) {
        csilk_ai_chat_request_t req = {
            .model = model ? model : "default",
            .messages = msgs,
            .message_count = msg_count,
            .stream = on_chunk ? true : false,
            .on_chunk = on_chunk,
            .user_data = user_data,
            .tools = tools,
            .tool_count = tool_count,
            .tool_choice = (tools && tool_count > 0) ? "auto" : "none",
        };

        csilk_ai_chat_response_t res = {0};
        if (csilk_ai_chat(ai, &req, &res) != 0) {
            csilk_ai_chat_response_free(&res);
            break;
        }

        /* If model produced final text and no further tool calls */
        if (res.tool_call_count == 0) {
            *out_final_text = res.content ? strdup(res.content) : strdup("");
            csilk_ai_chat_response_free(&res);
            rc = 0;
            break;
        }

        /* Handle tool calls */
        size_t n_calls = res.tool_call_count;

        /* Ensure room for assistant message + tool messages */
        if (msg_count + n_calls + 2 >= msg_capacity) {
            msg_capacity = msg_capacity * 2 + n_calls;
            csilk_ai_message_t* new_msgs = realloc(msgs, sizeof(csilk_ai_message_t) * msg_capacity);
            if (!new_msgs) {
                csilk_ai_chat_response_free(&res);
                break;
            }
            msgs = new_msgs;
        }

        /* Append assistant message containing tool_calls */
        size_t asst_idx = msg_count++;
        msgs[asst_idx] = (csilk_ai_message_t){
            .role = "assistant",
            .content = res.content ? strdup(res.content) : strdup(""),
            .tool_call_id = NULL,
            .tool_call_count = n_calls,
            .tool_calls = calloc(n_calls, sizeof(csilk_ai_tool_call_t)),
        };

        for (size_t j = 0; j < n_calls; j++) {
            msgs[asst_idx].tool_calls[j].id =
                strdup(res.tool_calls[j].id ? res.tool_calls[j].id : "");
            msgs[asst_idx].tool_calls[j].name =
                strdup(res.tool_calls[j].name ? res.tool_calls[j].name : "");
            msgs[asst_idx].tool_calls[j].arguments =
                strdup(res.tool_calls[j].arguments ? res.tool_calls[j].arguments : "{}");
        }

        /* Execute each tool and append tool observation messages */
        for (size_t t = 0; t < n_calls; t++) {
            /* Guard against use-after-free: capture IDs and arguments safely */
            char* tc_id = strdup(msgs[asst_idx].tool_calls[t].id);
            char* tc_name = msgs[asst_idx].tool_calls[t].name;
            char* tc_args = msgs[asst_idx].tool_calls[t].arguments;

            char* result_str = NULL;
            if (reg) {
                result_str = csilk_ai_tool_dispatch(reg, tc_name, tc_args);
            }
            if (!result_str) {
                result_str = strdup("{\"error\":\"tool dispatch failed\"}");
            }

            size_t tool_msg_idx = msg_count++;
            msgs[tool_msg_idx] = (csilk_ai_message_t){
                .role = "tool",
                .content = result_str,
                .tool_call_id = tc_id,
                .tool_calls = NULL,
                .tool_call_count = 0,
            };
        }

        csilk_ai_chat_response_free(&res);
    }

    free_conversation_messages(msgs, msg_count);
    if (tools) {
        csilk_ai_tools_free(tools, tool_count);
    }
    return rc;
}
