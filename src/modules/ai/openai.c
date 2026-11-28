/**
 * @file ai_openai.c
 * @brief OpenAI-compatible driver for the AI unified interface.
 *
 * Implements the csilk_ai_driver_t vtable for the OpenAI Chat Completions and
 * Embeddings APIs.  Also compatible with any OpenAI-API-proxy (Azure, Together,
 * local推理 servers) since it only relies on the standard endpoint shapes:
 *   - POST /chat/completions  (with optional SSE streaming)
 *   - POST /embeddings
 *
 * Key design points:
 *   - Streaming uses libcurl's write callback to parse SSE "data: " lines.
 *   - Tool / function calling is fully supported.
 *   - Token usage is read from the "usage" object in non-streaming responses.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/ai.h"
#include "csilk/csilk.h"
#include "modules/common/curl_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"

/** @brief Per-instance state for the OpenAI-compatible driver. */
typedef struct {
    char* api_key;  /**< Bearer token sent as Authorization header. */
    char* base_url; /**< API root (e.g., https://api.openai.com/v1). */
} openai_state_t;

/**
 * @brief Initialize the OpenAI driver state.
 * @note api_key is required (returns NULL if absent).
 * Allocates state and performs one-shot libcurl global init.
 */
static void*
openai_init(const char* api_key, const char* base_url)
{
    if (!api_key) {
        return NULL;
    }
    openai_state_t* state = malloc(sizeof(openai_state_t));
    if (!state) {
        return NULL;
    }

    state->api_key = strdup(api_key);
    state->base_url = strdup(base_url ? base_url : "https://api.openai.com/v1");

    /* Thread-safe one-shot libcurl global init */
    csilk_curl_global_init_once();

    return state;
}

/**
 * @brief Free the OpenAI driver state and associated resources.
 */
static void
openai_free(void* state_ptr)
{
    openai_state_t* state = (openai_state_t*)state_ptr;
    if (!state) {
        return;
    }
    free(state->api_key);
    free(state->base_url);
    free(state);
}

/**
 * @brief Per-request context passed to the streaming write callback.
 * Holds both the request (for on_chunk callback) and response (to accumulate
 * final content), plus a line-buffer for SSE frame reassembly.
 */
struct curl_context {
    const csilk_ai_chat_request_t* req;
    csilk_ai_chat_response_t*      res;
};

/**
 * @brief Process a single SSE line from the streaming response.
 *
 * Expects the standard OpenAI SSE format:
 *   data: {"choices":[{"delta":{"content":"text"}}]}
 * The special "[DONE]" token signals stream completion.
 *
 * For each delta chunk, the function:
 *   1. Captures stream-level token usage statistics if present.
 *   2. Appends content to the accumulated response and fires on_chunk callback.
 *   3. Reassembles streamed tool calls dynamically across chunks.
 */
static void
openai_stream_line_cb(const char* line, void* user_data)
{
    struct curl_context* ctx = (struct curl_context*)user_data;
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    /* Only process "data:" lines; ignore event type lines, empty keepalives */
    if (strncmp(line, "data:", 5) != 0) {
        return;
    }
    const char* data = line + 5;
    if (*data == ' ') {
        data++;
    }

    /* End-of-stream sentinel */
    if (strcmp(data, "[DONE]") == 0) {
        return;
    }

    csilk_json_t* root = csilk_json_parse(data);
    if (!root) {
        return;
    }

    /* 1. Capture stream-level token usage statistics */
    csilk_json_t* usage = csilk_json_get(root, "usage");
    if (usage && !csilk_json_is_null(usage)) {
        csilk_json_t* pt = csilk_json_get(usage, "prompt_tokens");
        csilk_json_t* ct = csilk_json_get(usage, "completion_tokens");
        csilk_json_t* tt = csilk_json_get(usage, "total_tokens");
        if (pt) {
            ctx->res->prompt_tokens = (int)csilk_json_int_value(pt);
        }
        if (ct) {
            ctx->res->completion_tokens = (int)csilk_json_int_value(ct);
        }
        if (tt) {
            ctx->res->total_tokens = (int)csilk_json_int_value(tt);
        }
    }

    /* 2. Process choices deltas */
    csilk_json_t* choices = csilk_json_get(root, "choices");
    if (csilk_json_is_array(choices) && csilk_json_array_size(choices) > 0) {
        csilk_json_t* first = csilk_json_array_get(choices, 0);
        csilk_json_t* delta = csilk_json_get(first, "delta");
        if (delta) {
            /* 2a. Real-time text token dispatch */
            csilk_json_t* content = csilk_json_get(delta, "content");
            if (csilk_json_is_string(content)) {
                const char* chunk_str = csilk_json_string_value(content);
                size_t      clen = strlen(chunk_str);
                if (clen > 0) {
                    size_t current_len = ctx->res->content ? strlen(ctx->res->content) : 0;
                    char*  new_content = realloc(ctx->res->content, current_len + clen + 1);
                    if (new_content) {
                        ctx->res->content = new_content;
                        memcpy(ctx->res->content + current_len, chunk_str, clen);
                        ctx->res->content[current_len + clen] = '\0';
                    }

                    /* Forward chunk to user streaming callback */
                    if (ctx->req->on_chunk) {
                        ctx->req->on_chunk(chunk_str, ctx->req->user_data);
                    }
                }
            }

            /* 2b. Real-time reasoning/thinking token dispatch */
            csilk_json_t* reasoning = csilk_json_get(delta, "reasoning_content");
            if (!reasoning) {
                reasoning = csilk_json_get(delta, "reasoning");
            }
            if (csilk_json_is_string(reasoning)) {
                const char* r_str = csilk_json_string_value(reasoning);
                size_t      rlen = strlen(r_str);
                if (rlen > 0) {
                    size_t cur_rlen =
                        ctx->res->reasoning_content ? strlen(ctx->res->reasoning_content) : 0;
                    char* new_r = realloc(ctx->res->reasoning_content, cur_rlen + rlen + 1);
                    if (new_r) {
                        ctx->res->reasoning_content = new_r;
                        memcpy(ctx->res->reasoning_content + cur_rlen, r_str, rlen);
                        ctx->res->reasoning_content[cur_rlen + rlen] = '\0';
                    }

                    /* Forward reasoning chunk to user streaming callback */
                    if (ctx->req->on_reasoning_chunk) {
                        ctx->req->on_reasoning_chunk(r_str, ctx->req->user_data);
                    }
                }
            }

            /* 2c. Streamed Tool Calling Reassembly (delta.tool_calls) */
            csilk_json_t* tool_calls = csilk_json_get(delta, "tool_calls");
            if (csilk_json_is_array(tool_calls)) {
                size_t tc_count = csilk_json_array_size(tool_calls);
                for (size_t i = 0; i < tc_count; i++) {
                    csilk_json_t* tc = csilk_json_array_get(tool_calls, i);
                    if (!tc) {
                        continue;
                    }

                    csilk_json_t* idx_json = csilk_json_get(tc, "index");
                    size_t        idx = 0;
                    if (idx_json) {
                        int raw_idx = csilk_json_int_value(idx_json);
                        if (raw_idx < 0 || raw_idx >= 256) {
                            continue;
                        }
                        idx = (size_t)raw_idx;
                    } else if (ctx->res->tool_call_count > 0) {
                        idx = ctx->res->tool_call_count - 1;
                    }

                    /* Expand tool_calls array if needed */
                    if (idx >= ctx->res->tool_call_count) {
                        size_t                new_count = idx + 1;
                        csilk_ai_tool_call_t* new_tcs =
                            realloc(ctx->res->tool_calls, new_count * sizeof(csilk_ai_tool_call_t));
                        if (new_tcs) {
                            for (size_t k = ctx->res->tool_call_count; k < new_count; k++) {
                                memset(&new_tcs[k], 0, sizeof(csilk_ai_tool_call_t));
                            }
                            ctx->res->tool_calls = new_tcs;
                            ctx->res->tool_call_count = new_count;
                        } else {
                            continue;
                        }
                    }

                    /* Capture function id */
                    csilk_json_t* fid = csilk_json_get(tc, "id");
                    if (fid && csilk_json_is_string(fid)) {
                        const char* id_str = csilk_json_string_value(fid);
                        if (id_str && !ctx->res->tool_calls[idx].id) {
                            ctx->res->tool_calls[idx].id = strdup(id_str);
                        } else if (id_str) {
                            size_t cur_id_len = strlen(ctx->res->tool_calls[idx].id);
                            size_t id_len = strlen(id_str);
                            size_t total_id_len = cur_id_len + id_len + 1;
                            char*  new_id = realloc(ctx->res->tool_calls[idx].id, total_id_len);
                            if (new_id) {
                                ctx->res->tool_calls[idx].id = new_id;
                                memcpy(new_id + cur_id_len, id_str, id_len);
                                new_id[cur_id_len + id_len] = '\0';
                            }
                        }
                    }

                    /* Capture function name & arguments */
                    csilk_json_t* func = csilk_json_get(tc, "function");
                    if (func) {
                        csilk_json_t* fname = csilk_json_get(func, "name");
                        if (fname && csilk_json_is_string(fname)) {
                            const char* name_str = csilk_json_string_value(fname);
                            if (name_str && !ctx->res->tool_calls[idx].name) {
                                ctx->res->tool_calls[idx].name = strdup(name_str);
                            } else if (name_str) {
                                size_t cur_name_len = strlen(ctx->res->tool_calls[idx].name);
                                size_t name_len = strlen(name_str);
                                size_t total_name_len = cur_name_len + name_len + 1;
                                char*  new_name =
                                    realloc(ctx->res->tool_calls[idx].name, total_name_len);
                                if (new_name) {
                                    ctx->res->tool_calls[idx].name = new_name;
                                    memcpy(new_name + cur_name_len, name_str, name_len);
                                    new_name[cur_name_len + name_len] = '\0';
                                }
                            }
                        }

                        csilk_json_t* fargs = csilk_json_get(func, "arguments");
                        if (fargs && csilk_json_is_string(fargs)) {
                            const char* arg_chunk = csilk_json_string_value(fargs);
                            size_t      alen = arg_chunk ? strlen(arg_chunk) : 0;
                            if (alen > 0) {
                                size_t cur_alen = ctx->res->tool_calls[idx].arguments
                                                      ? strlen(ctx->res->tool_calls[idx].arguments)
                                                      : 0;
                                size_t total_args_len = cur_alen + alen + 1;
                                char*  new_args =
                                    realloc(ctx->res->tool_calls[idx].arguments, total_args_len);
                                if (new_args) {
                                    ctx->res->tool_calls[idx].arguments = new_args;
                                    memcpy(new_args + cur_alen, arg_chunk, alen);
                                    new_args[cur_alen + alen] = '\0';
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    csilk_json_free(root);
}

/**
 * @brief libcurl write callback that handles both streaming and non-streaming
 *        responses.
 *
 * Non-streaming path:
 *   Appends raw bytes to ctx->body (same as write_cb_simple).
 *
 * Streaming path (SSE):
 *   Appends bytes to a line reassembly buffer (ctx->line_buf), then
 *   extracts complete lines delimited by '\n'. Each complete line is
 *   forwarded to process_stream_line(). A trailing partial line is
 *   preserved in the buffer for the next callback invocation.
 *
 * @return Number of bytes consumed, or 0 to abort on OOM.
 */
static int
openai_chat(void* state_ptr, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    openai_state_t* state = (openai_state_t*)state_ptr;
    if (!state || !req || !res) {
        return -1;
    }

    /* --- Step 1: Build JSON request body --- */
    csilk_json_t* root = csilk_json_object();
    if (!root) {
        res->error_message = strdup("Failed to allocate AI request JSON");
        return -1;
    }
    if (!csilk_json_add_string(root, "model", req->model ? req->model : "gpt-3.5-turbo")) {
        csilk_json_free(root);
        res->error_message = strdup("Failed to populate AI request JSON");
        return -1;
    }

    csilk_json_t* msgs = csilk_json_array();
    if (!msgs) {
        csilk_json_free(root);
        res->error_message = strdup("Failed to allocate AI messages JSON");
        return -1;
    }
    for (size_t i = 0; i < req->message_count; i++) {
        const csilk_ai_message_t* m = &req->messages[i];
        csilk_json_t*             obj = csilk_json_object();
        csilk_json_add_string(obj, "role", m->role ? m->role : "user");
        if (m->role && strcmp(m->role, "tool") == 0) {
            /* role=tool messages carry tool_call_id at top level */
            csilk_json_add_string(obj, "tool_call_id", m->tool_call_id ? m->tool_call_id : "");
            csilk_json_add_string(obj, "content", m->content ? m->content : "");
        } else if (m->tool_call_count > 0) {
            /* role=assistant with tool_calls: build the tool_calls array */
            csilk_json_add_string(obj, "content", m->content ? m->content : "");
            csilk_json_t* tc_arr = csilk_json_array();
            for (size_t j = 0; j < m->tool_call_count; j++) {
                const csilk_ai_tool_call_t* tc = &m->tool_calls[j];
                csilk_json_t*               tc_obj = csilk_json_object();
                csilk_json_add_string(tc_obj, "id", tc->id ? tc->id : "");
                csilk_json_add_string(tc_obj, "type", "function");
                csilk_json_t* fn = csilk_json_object();
                csilk_json_add_string(fn, "name", tc->name ? tc->name : "");
                csilk_json_add_string(fn, "arguments", tc->arguments ? tc->arguments : "{}");
                csilk_json_add_object(tc_obj, "function", fn);
                csilk_json_array_append(tc_arr, tc_obj);
            }
            csilk_json_add_array(obj, "tool_calls", tc_arr);
        } else {
            csilk_json_add_string(obj, "content", m->content ? m->content : "");
        }
        csilk_json_array_append(msgs, obj);
    }
    csilk_json_add_object(root, "messages", msgs);

    /* Sampling params (only set when non-zero/non-default) */
    if (req->temperature > 0) {
        csilk_json_add_number(root, "temperature", req->temperature);
    }
    if (req->top_p > 0) {
        csilk_json_add_number(root, "top_p", req->top_p);
    }
    if (req->presence_penalty != 0) {
        csilk_json_add_number(root, "presence_penalty", req->presence_penalty);
    }
    if (req->frequency_penalty != 0) {
        csilk_json_add_number(root, "frequency_penalty", req->frequency_penalty);
    }
    if (req->max_tokens > 0) {
        csilk_json_add_number(root, "max_tokens", req->max_tokens);
    }
    if (req->user) {
        csilk_json_add_string(root, "user", req->user);
    }
    if (req->stream) {
        csilk_json_add_bool(root, "stream", true);
        csilk_json_t* stream_opts = csilk_json_object();
        csilk_json_add_bool(stream_opts, "include_usage", true);
        csilk_json_add_object(root, "stream_options", stream_opts);
    }

    /* Stop sequences array */
    if (req->stop_count > 0) {
        csilk_json_t* stop = csilk_json_array();
        for (size_t i = 0; i < req->stop_count; i++) {
            csilk_json_array_append(stop, csilk_json_string_new(req->stop[i]));
        }
        csilk_json_add_object(root, "stop", stop);
    }

    /* Tool / function definitions */
    if (req->tool_count > 0) {
        csilk_json_t* tools = csilk_json_array();
        for (size_t i = 0; i < req->tool_count; i++) {
            csilk_json_t* t = csilk_json_object();
            csilk_json_add_string(t, "type", req->tools[i].type);
            csilk_json_t* f = csilk_json_object();
            csilk_json_add_string(f, "name", req->tools[i].function.name);
            if (req->tools[i].function.description) {
                csilk_json_add_string(f, "description", req->tools[i].function.description);
            }
            if (req->tools[i].function.parameters_json) {
                csilk_json_add_object(
                    f,
                    "parameters",
                    csilk_json_copy((csilk_json_t*)req->tools[i].function.parameters_json));
            }
            csilk_json_add_object(t, "function", f);
            csilk_json_array_append(tools, t);
        }
        csilk_json_add_object(root, "tools", tools);
        if (req->tool_choice) {
            csilk_json_add_string(root, "tool_choice", req->tool_choice);
        }
    }

    /* Extended thinking / reasoning effort (o1, o3, GPT-5, etc.) */
    if (req->reasoning_effort) {
        csilk_json_add_string(root, "reasoning_effort", req->reasoning_effort);
    }

    /* Thinking / reasoning mode control (Agnes, Qwen, vLLM chat_template_kwargs) */
    if (req->chat_template_kwargs || req->has_enable_thinking) {
        csilk_json_t* ctk = req->chat_template_kwargs
                                ? csilk_json_copy((csilk_json_t*)req->chat_template_kwargs)
                                : csilk_json_object();
        if (req->has_enable_thinking) {
            csilk_json_add_bool(ctk, "enable_thinking", req->enable_thinking);
        }
        csilk_json_add_object(root, "chat_template_kwargs", ctk);
    }

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        res->error_message = strdup("Failed to serialize AI request JSON");
        return -1;
    }

    /* --- Step 2: Prepare and send HTTP request --- */
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", state->base_url);

    struct curl_slist* headers = NULL;
    char               auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", state->api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_hdr);

    /* Bounded time-outs: streaming responses are excluded from the total cap
     * so long LLM streams are unaffected. */
    long total_timeout = 0;
    if (req->timeout_ms > 0) {
        total_timeout = (long)req->timeout_ms;
    } else if (!req->stream) {
        total_timeout = 300L * 1000L;
    }

    struct curl_context      ctx = {.req = req, .res = res};
    csilk_curl_buf_t         resp_buf = {0};
    csilk_curl_line_stream_t stream = {
        .on_line = openai_stream_line_cb,
        .user_data = &ctx,
    };
    long http_code = 0;
    char curl_err[256] = {0};
    int  perf_rc;
    if (req->stream) {
        perf_rc = csilk_curl_post_stream(url,
                                         headers,
                                         json_body,
                                         10L * 1000L,
                                         total_timeout,
                                         &stream,
                                         &http_code,
                                         curl_err,
                                         sizeof(curl_err));
        csilk_curl_line_stream_free(&stream);
    } else {
        perf_rc = csilk_curl_request_json("POST",
                                          url,
                                          headers,
                                          json_body,
                                          10L * 1000L,
                                          total_timeout,
                                          &resp_buf,
                                          &http_code,
                                          curl_err,
                                          sizeof(curl_err));
    }

    free(json_body);
    curl_slist_free_all(headers);

    /* Transport or SSRF failure */
    if (perf_rc != 0) {
        res->error_message = strdup(curl_err[0] ? curl_err : "CURL error");
        free(resp_buf.data);
        return -1;
    }

    /* HTTP-level failure */
    if (http_code >= 400) {
        char err_buf[512];
        snprintf(err_buf,
                 sizeof(err_buf),
                 "HTTP error %ld: %s",
                 http_code,
                 resp_buf.data ? resp_buf.data : "(no response body)");
        res->error_message = strdup(err_buf);
        free(resp_buf.data);
        return -1;
    }

    /* --- Step 4a: Streaming path -- content or tool calls already built by stream cb --- */
    if (req->stream) {
        if (res->total_tokens == 0 && (res->prompt_tokens > 0 || res->completion_tokens > 0)) {
            res->total_tokens = res->prompt_tokens + res->completion_tokens;
        }
        if (!res->content && res->tool_call_count == 0) {
            res->content = strdup("");
        }
        return 0;
    }

    /* --- Step 4b: Non-streaming path -- parse the JSON response --- */
    csilk_json_t* resp_root = csilk_json_parse(resp_buf.data);
    if (resp_root) {
        res->raw_response = resp_buf.data;
        csilk_json_t* choices = csilk_json_get(resp_root, "choices");
        if (csilk_json_is_array(choices) && csilk_json_array_size(choices) > 0) {
            csilk_json_t* first = csilk_json_array_get(choices, 0);
            csilk_json_t* msg = csilk_json_get(first, "message");
            csilk_json_t* content = csilk_json_get(msg, "content");
            if (csilk_json_is_string(content)) {
                res->content = strdup(csilk_json_string_value(content));
            }
            csilk_json_t* reasoning = csilk_json_get(msg, "reasoning_content");
            if (!reasoning) {
                reasoning = csilk_json_get(msg, "reasoning");
            }
            if (csilk_json_is_string(reasoning)) {
                res->reasoning_content = strdup(csilk_json_string_value(reasoning));
            }
            /* Extract tool calls if the model requested function invocations */
            csilk_json_t* tcalls = csilk_json_get(msg, "tool_calls");
            if (csilk_json_is_array(tcalls)) {
                size_t raw_cnt = csilk_json_array_size(tcalls);
                if (raw_cnt > 1024) {
                    raw_cnt = 1024;
                }
                res->tool_calls = calloc(raw_cnt ? raw_cnt : 1, sizeof(csilk_ai_tool_call_t));
                if (res->tool_calls) {
                    res->tool_call_count = raw_cnt;
                    for (size_t i = 0; i < res->tool_call_count; i++) {
                        csilk_json_t* tc = csilk_json_array_get(tcalls, i);
                        if (!tc) {
                            continue;
                        }
                        csilk_json_t* fid = csilk_json_get(tc, "id");
                        csilk_json_t* func = csilk_json_get(tc, "function");
                        csilk_json_t* fname = func ? csilk_json_get(func, "name") : NULL;
                        csilk_json_t* fargs = func ? csilk_json_get(func, "arguments") : NULL;

                        if (fid && csilk_json_is_string(fid)) {
                            const char* s = csilk_json_string_value(fid);
                            if (s) {
                                res->tool_calls[i].id = strdup(s);
                            }
                        }
                        if (fname && csilk_json_is_string(fname)) {
                            const char* s = csilk_json_string_value(fname);
                            if (s) {
                                res->tool_calls[i].name = strdup(s);
                            }
                        }
                        if (fargs && csilk_json_is_string(fargs)) {
                            const char* s = csilk_json_string_value(fargs);
                            if (s) {
                                res->tool_calls[i].arguments = strdup(s);
                            }
                        }
                    }
                }
            }
        }
        /* Token usage statistics */
        csilk_json_t* usage = csilk_json_get(resp_root, "usage");
        if (usage) {
            res->prompt_tokens = csilk_json_get_int(usage, "prompt_tokens");
            res->completion_tokens = csilk_json_get_int(usage, "completion_tokens");
            res->total_tokens = csilk_json_get_int(usage, "total_tokens");
        }
        csilk_json_free(resp_root);
    } else {
        free(resp_buf.data);
    }

    return (res->content != NULL || res->reasoning_content != NULL || res->tool_call_count > 0)
               ? 0
               : -1;
}

/**
 * @brief Generate embeddings for a batch of input strings.
 *
 * Flow:
 *   1. Build a JSON body with the model name and "input" array.
 *   2. POST to <base_url>/embeddings with Bearer auth.
 *   3. Parse the "data" array -- each entry contains an "embedding" vector.
 *      Vectors are flattened into a single float[] in input order.
 *   4. Extract usage statistics if present.
 *
 * @note The response dimension is inferred from the first embedding vector.
 *       All vectors in a batch must have the same dimension.
 */
static int
openai_embeddings(void*                           state_ptr,
                  const char*                     model,
                  const char**                    input,
                  size_t                          count,
                  csilk_ai_embeddings_response_t* res)
{
    openai_state_t* state = (openai_state_t*)state_ptr;
    if (!state || !model || !input || !res) {
        return -1;
    }

    /* --- Build the JSON request body --- */
    csilk_json_t* root = csilk_json_object();
    csilk_json_add_string(root, "model", model);
    csilk_json_t* in_arr = csilk_json_array();
    for (size_t i = 0; i < count; i++) {
        csilk_json_array_append(in_arr, csilk_json_string_new(input[i]));
    }
    csilk_json_add_object(root, "input", in_arr);

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        res->error_message = strdup("Failed to serialize AI request JSON");
        return -1;
    }

    /* --- Prepare and perform the HTTP request --- */
    char url[512];
    snprintf(url, sizeof(url), "%s/embeddings", state->base_url);

    struct curl_slist* headers = NULL;
    char               auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", state->api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_hdr);

    csilk_curl_buf_t cr = {0};
    long             http_code = 0;
    char             err[256] = {0};
    int              rc = csilk_curl_request_json("POST",
                                     url,
                                     headers,
                                     json_body,
                                     10L * 1000L,
                                     300L * 1000L,
                                     &cr,
                                     &http_code,
                                     err,
                                     sizeof(err));
    free(json_body);
    curl_slist_free_all(headers);

    if (rc != 0) {
        res->error_message = strdup(err[0] ? err : "CURL error");
        free(cr.data);
        return -1;
    }

    /* --- Parse the response and extract vectors --- */
    csilk_json_t* resp = csilk_json_parse(cr.data);
    free(cr.data);
    if (!resp) {
        res->error_message = strdup("JSON parse error");
        return -1;
    }

    csilk_json_t* data = csilk_json_get(resp, "data");
    if (csilk_json_is_array(data)) {
        res->count = csilk_json_array_size(data);
        if (res->count > 0) {
            csilk_json_t* first = csilk_json_array_get(data, 0);
            csilk_json_t* vec = csilk_json_get(first, "embedding");
            if (csilk_json_is_array(vec)) {
                /* Flatten all embedding vectors into a single float array */
                res->dimension = csilk_json_array_size(vec);
                res->values = malloc(sizeof(float) * res->count * res->dimension);
                for (size_t i = 0; i < res->count; i++) {
                    csilk_json_t* item = csilk_json_array_get(data, i);
                    csilk_json_t* v = csilk_json_get(item, "embedding");
                    for (size_t j = 0; j < res->dimension; j++) {
                        res->values[i * res->dimension + j] =
                            (float)csilk_json_number_value(csilk_json_array_get(v, j));
                    }
                }
            }
        }
    }

    csilk_json_t* usage = csilk_json_get(resp, "usage");
    if (usage) {
        res->prompt_tokens = csilk_json_get_int(usage, "prompt_tokens");
        res->total_tokens = csilk_json_get_int(usage, "total_tokens");
    }

    csilk_json_free(resp);
    return 0;
}

/** @brief Driver vtable for the OpenAI-compatible AI backend. */
static const csilk_ai_driver_t openai_driver = {
    .name = "openai",
    .init = openai_init,
    .chat = openai_chat,
    .embeddings = openai_embeddings,
    .free = openai_free,
};

/**
 * @brief Register the OpenAI driver with the AI subsystem.
 * Called during startup to make "openai" available to csilk_ai_new().
 */
void
csilk_ai_openai_init_driver(void)
{
    csilk_ai_register_driver(&openai_driver);
}
