/**
 * @file anthropic.c
 * @brief Native Anthropic Messages API (Claude) driver for the AI interface.
 *
 * Implements the csilk_ai_driver_t vtable for Anthropic Claude models using the
 * Messages API (POST /v1/messages).
 *
 * Key features:
 *   - Native Anthropic JSON protocol: top-level "system" prompt, "messages" array,
 *     and "tools" with "input_schema".
 *   - Both non-streaming and Server-Sent Events (SSE) streaming modes.
 *   - Streamed tool use with dynamic argument assembly across chunks.
 *   - Extended thinking / reasoning extraction (CoT) into reasoning_content.
 *   - Prompt and completion token usage tracking.
 *   - Registered as both "anthropic" and "claude".
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"
#include "modules/common/curl_common.h"

#define ANTHROPIC_DEFAULT_BASE_URL "https://api.anthropic.com/v1"
#define ANTHROPIC_API_VERSION "2023-06-01"
#define ANTHROPIC_BETA_FEATURES "prompt-caching-2024-07-25"

typedef struct {
    char* api_key;
    char* base_url;
} anthropic_state_t;

static void*
anthropic_init(const char* api_key, const char* base_url)
{
    if (!api_key || !api_key[0]) {
        return NULL;
    }
    anthropic_state_t* state = calloc(1, sizeof(anthropic_state_t));
    if (!state) {
        return NULL;
    }

    state->api_key = strdup(api_key);
    state->base_url = strdup(base_url ? base_url : ANTHROPIC_DEFAULT_BASE_URL);

    csilk_curl_global_init_once();
    return state;
}

static void
anthropic_free(void* state_ptr)
{
    anthropic_state_t* state = (anthropic_state_t*)state_ptr;
    if (!state) {
        return;
    }
    free(state->api_key);
    free(state->base_url);
    free(state);
}

/* Streaming accumulator for tool calls and text blocks */
typedef struct {
    char*  id;
    char*  name;
    char*  args_buf;
    size_t args_len;
    size_t args_cap;
} streamed_tool_t;

struct anthropic_stream_ctx {
    const csilk_ai_chat_request_t* req;
    csilk_ai_chat_response_t*      res;
    streamed_tool_t*               tools;
    size_t                         tool_count;
    size_t                         tool_capacity;
};

static void
streamed_tool_append_args(streamed_tool_t* t, const char* chunk, size_t clen)
{
    if (!chunk || clen == 0) {
        return;
    }
    if (t->args_len + clen + 1 >= t->args_cap) {
        size_t ncap = t->args_cap == 0 ? 256 : t->args_cap * 2;
        while (t->args_len + clen + 1 >= ncap) {
            ncap *= 2;
        }
        char* nb = realloc(t->args_buf, ncap);
        if (!nb) {
            return;
        }
        t->args_buf = nb;
        t->args_cap = ncap;
    }
    memcpy(t->args_buf + t->args_len, chunk, clen);
    t->args_len += clen;
    t->args_buf[t->args_len] = '\0';
}

static void
parse_sse_event(struct anthropic_stream_ctx* ctx, const char* json_str)
{
    csilk_json_t* js = csilk_json_parse(json_str);
    if (!js) {
        return;
    }

    const char* type = csilk_json_get_string(js, "type");
    if (!type) {
        csilk_json_free(js);
        return;
    }

    if (strcmp(type, "message_start") == 0) {
        csilk_json_t* msg = csilk_json_get_object(js, "message");
        if (msg) {
            csilk_json_t* usage = csilk_json_get_object(msg, "usage");
            if (usage) {
                ctx->res->prompt_tokens = (int)csilk_json_get_int(usage, "input_tokens");
            }
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        int64_t       idx = csilk_json_get_int(js, "index");
        csilk_json_t* cb = csilk_json_get_object(js, "content_block");
        if (cb) {
            const char* cb_type = csilk_json_get_string(cb, "type");
            if (cb_type && strcmp(cb_type, "tool_use") == 0) {
                const char* id = csilk_json_get_string(cb, "id");
                const char* name = csilk_json_get_string(cb, "name");
                if (ctx->tool_count >= ctx->tool_capacity) {
                    size_t           ncap = ctx->tool_capacity == 0 ? 4 : ctx->tool_capacity * 2;
                    streamed_tool_t* nt = realloc(ctx->tools, ncap * sizeof(streamed_tool_t));
                    if (nt) {
                        ctx->tools = nt;
                        ctx->tool_capacity = ncap;
                    }
                }
                if (ctx->tool_count < ctx->tool_capacity) {
                    streamed_tool_t* t = &ctx->tools[ctx->tool_count++];
                    t->id = id ? strdup(id) : NULL;
                    t->name = name ? strdup(name) : NULL;
                    t->args_buf = NULL;
                    t->args_len = 0;
                    t->args_cap = 0;
                }
            }
        }
        (void)idx;
    } else if (strcmp(type, "content_block_delta") == 0) {
        csilk_json_t* delta = csilk_json_get_object(js, "delta");
        if (delta) {
            const char* delta_type = csilk_json_get_string(delta, "type");
            if (delta_type && strcmp(delta_type, "text_delta") == 0) {
                const char* txt = csilk_json_get_string(delta, "text");
                if (txt && txt[0]) {
                    if (ctx->req->on_chunk) {
                        ctx->req->on_chunk(txt, ctx->req->user_data);
                    }
                    size_t clen = strlen(txt);
                    size_t cur_len = ctx->res->content ? strlen(ctx->res->content) : 0;
                    char*  nb = realloc(ctx->res->content, cur_len + clen + 1);
                    if (nb) {
                        memcpy(nb + cur_len, txt, clen);
                        nb[cur_len + clen] = '\0';
                        ctx->res->content = nb;
                    }
                }
            } else if (delta_type && strcmp(delta_type, "thinking_delta") == 0) {
                const char* th = csilk_json_get_string(delta, "thinking");
                if (th && th[0]) {
                    if (ctx->req->on_reasoning_chunk) {
                        ctx->req->on_reasoning_chunk(th, ctx->req->user_data);
                    }
                    size_t tlen = strlen(th);
                    size_t cur_len =
                        ctx->res->reasoning_content ? strlen(ctx->res->reasoning_content) : 0;
                    char* nb = realloc(ctx->res->reasoning_content, cur_len + tlen + 1);
                    if (nb) {
                        memcpy(nb + cur_len, th, tlen);
                        nb[cur_len + tlen] = '\0';
                        ctx->res->reasoning_content = nb;
                    }
                }
            } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
                const char* part = csilk_json_get_string(delta, "partial_json");
                if (part && ctx->tool_count > 0) {
                    streamed_tool_append_args(&ctx->tools[ctx->tool_count - 1], part, strlen(part));
                }
            }
        }
    } else if (strcmp(type, "message_delta") == 0) {
        csilk_json_t* usage = csilk_json_get_object(js, "usage");
        if (usage) {
            ctx->res->completion_tokens = (int)csilk_json_get_int(usage, "output_tokens");
            ctx->res->total_tokens = ctx->res->prompt_tokens + ctx->res->completion_tokens;
        }
    }

    csilk_json_free(js);
}

static void
anthropic_stream_line_cb(const char* line, void* user_data)
{
    struct anthropic_stream_ctx* ctx = (struct anthropic_stream_ctx*)user_data;
    if (strncmp(line, "data: ", 6) == 0) {
        const char* payload = line + 6;
        while (*payload == ' ') {
            payload++;
        }
        parse_sse_event(ctx, payload);
    }
}

static int
anthropic_chat(void* state_ptr, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    anthropic_state_t* state = (anthropic_state_t*)state_ptr;
    if (!state || !req || !res) {
        return -1;
    }

    /* Build endpoint URL */
    char   url[512];
    size_t ulen = strlen(state->base_url);
    if (ulen >= 9 && strcmp(state->base_url + ulen - 9, "/messages") == 0) {
        snprintf(url, sizeof(url), "%s", state->base_url);
    } else {
        snprintf(url, sizeof(url), "%s/messages", state->base_url);
    }

    /* Top-level JSON request body */
    csilk_json_t* root = csilk_json_object();
    if (!root) {
        return -1;
    }

    csilk_json_add_string(
        root, "model", (req->model && req->model[0]) ? req->model : "claude-3-5-sonnet-20241022");
    csilk_json_add_int(root, "max_tokens", req->max_tokens > 0 ? req->max_tokens : 4096);
    if (req->temperature > 0.0) {
        csilk_json_add_number(root, "temperature", req->temperature);
    }
    if (req->stream) {
        csilk_json_add_bool(root, "stream", true);
    }

    /* Separate top-level "system" prompt from conversation messages */
    char*         system_buf = NULL;
    size_t        sys_len = 0;
    csilk_json_t* msgs_arr = csilk_json_array();

    for (size_t i = 0; i < req->message_count; i++) {
        const char* role = req->messages[i].role;
        const char* content = req->messages[i].content ? req->messages[i].content : "";
        if (role && strcmp(role, "system") == 0) {
            size_t clen = strlen(content);
            char*  nb = realloc(system_buf, sys_len + clen + 2);
            if (nb) {
                if (sys_len > 0) {
                    nb[sys_len++] = '\n';
                }
                memcpy(nb + sys_len, content, clen);
                sys_len += clen;
                nb[sys_len] = '\0';
                system_buf = nb;
            }
        } else if (role && strcmp(role, "tool") == 0 && req->messages[i].tool_call_id) {
            csilk_json_t* m = csilk_json_object();
            csilk_json_add_string(m, "role", "user");
            csilk_json_t* blocks = csilk_json_array();
            csilk_json_t* tr = csilk_json_object();
            csilk_json_add_string(tr, "type", "tool_result");
            csilk_json_add_string(tr, "tool_use_id", req->messages[i].tool_call_id);
            csilk_json_add_string(tr, "content", content);
            csilk_json_array_append(blocks, tr);
            csilk_json_add_object(m, "content", blocks);
            csilk_json_array_append(msgs_arr, m);
        } else if (role && strcmp(role, "assistant") == 0 && req->messages[i].tool_calls &&
                   req->messages[i].tool_call_count > 0) {
            csilk_json_t* m = csilk_json_object();
            csilk_json_add_string(m, "role", "assistant");
            csilk_json_t* blocks = csilk_json_array();
            if (content[0] != '\0') {
                csilk_json_t* tb = csilk_json_object();
                csilk_json_add_string(tb, "type", "text");
                csilk_json_add_string(tb, "text", content);
                csilk_json_array_append(blocks, tb);
            }
            for (size_t tc = 0; tc < req->messages[i].tool_call_count; tc++) {
                csilk_json_t* tu = csilk_json_object();
                csilk_json_add_string(tu, "type", "tool_use");
                csilk_json_add_string(
                    tu,
                    "id",
                    req->messages[i].tool_calls[tc].id ? req->messages[i].tool_calls[tc].id : "");
                csilk_json_add_string(tu,
                                      "name",
                                      req->messages[i].tool_calls[tc].name
                                          ? req->messages[i].tool_calls[tc].name
                                          : "");
                csilk_json_t* in_obj =
                    req->messages[i].tool_calls[tc].arguments
                        ? csilk_json_parse(req->messages[i].tool_calls[tc].arguments)
                        : NULL;
                csilk_json_add_object(tu, "input", in_obj ? in_obj : csilk_json_object());
                csilk_json_array_append(blocks, tu);
            }
            csilk_json_add_object(m, "content", blocks);
            csilk_json_array_append(msgs_arr, m);
        } else {
            csilk_json_t* m = csilk_json_object();
            csilk_json_add_string(m, "role", (role && role[0]) ? role : "user");
            csilk_json_add_string(m, "content", content);
            csilk_json_array_append(msgs_arr, m);
        }
    }

    if (system_buf) {
        csilk_json_add_string(root, "system", system_buf);
        free(system_buf);
    }
    csilk_json_add_object(root, "messages", msgs_arr);

    /* Tools conversion: name, description, input_schema */
    if (req->tools && req->tool_count > 0) {
        csilk_json_t* tools_arr = csilk_json_array();
        for (size_t i = 0; i < req->tool_count; i++) {
            csilk_json_t* t = csilk_json_object();
            csilk_json_add_string(t, "name", req->tools[i].function.name);
            if (req->tools[i].function.description) {
                csilk_json_add_string(t, "description", req->tools[i].function.description);
            }
            if (req->tools[i].function.parameters_json) {
                csilk_json_t* pval = (csilk_json_t*)req->tools[i].function.parameters_json;
                if (csilk_json_is_object(pval)) {
                    csilk_json_add_object(t, "input_schema", csilk_json_copy(pval));
                } else if (csilk_json_is_string(pval)) {
                    const char*   s = csilk_json_string_value(pval);
                    csilk_json_t* pobj = s ? csilk_json_parse(s) : NULL;
                    csilk_json_add_object(t, "input_schema", pobj ? pobj : csilk_json_object());
                } else {
                    /* Might be raw JSON string passed directly */
                    csilk_json_t* pobj = csilk_json_parse((const char*)pval);
                    csilk_json_add_object(t, "input_schema", pobj ? pobj : csilk_json_object());
                }
            } else {
                csilk_json_add_object(t, "input_schema", csilk_json_object());
            }
            csilk_json_array_append(tools_arr, t);
        }
        csilk_json_add_object(root, "tools", tools_arr);
    }

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        return -1;
    }

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char key_hdr[256];
    snprintf(key_hdr, sizeof(key_hdr), "x-api-key: %s", state->api_key);
    headers = curl_slist_append(headers, key_hdr);
    headers = curl_slist_append(headers, "anthropic-version: " ANTHROPIC_API_VERSION);
    headers = curl_slist_append(headers, "anthropic-beta: " ANTHROPIC_BETA_FEATURES);

    long total_timeout = 0;
    if (req->timeout_ms > 0) {
        total_timeout = (long)req->timeout_ms;
    } else if (!req->stream) {
        total_timeout = 300000L;
    }

    csilk_curl_buf_t            resp_buf = {0};
    struct anthropic_stream_ctx sctx = {
        .req = req,
        .res = res,
        .tools = NULL,
        .tool_count = 0,
        .tool_capacity = 0,
    };
    csilk_curl_line_stream_t stream = {
        .on_line = anthropic_stream_line_cb,
        .user_data = &sctx,
    };
    long http_code = 0;
    char curl_err[256] = {0};
    int  perf_rc;
    if (req->stream) {
        perf_rc = csilk_curl_post_stream(url,
                                         headers,
                                         json_body,
                                         10000L,
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
                                          10000L,
                                          total_timeout,
                                          &resp_buf,
                                          &http_code,
                                          curl_err,
                                          sizeof(curl_err));
    }

    free(json_body);
    curl_slist_free_all(headers);

    if (perf_rc != 0) {
        free(resp_buf.data);
        for (size_t i = 0; i < sctx.tool_count; i++) {
            free(sctx.tools[i].id);
            free(sctx.tools[i].name);
            free(sctx.tools[i].args_buf);
        }
        free(sctx.tools);
        res->error_message = strdup(curl_err[0] ? curl_err : "CURL error");
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        const char* err = (req->stream || !resp_buf.data) ? "HTTP error" : resp_buf.data;
        res->error_message = strdup(err);
        free(resp_buf.data);
        for (size_t i = 0; i < sctx.tool_count; i++) {
            free(sctx.tools[i].id);
            free(sctx.tools[i].name);
            free(sctx.tools[i].args_buf);
        }
        free(sctx.tools);
        return -1;
    }

    if (req->stream) {
        free(resp_buf.data);
        if (sctx.tool_count > 0) {
            res->tool_calls = calloc(sctx.tool_count, sizeof(csilk_ai_tool_call_t));
            if (res->tool_calls) {
                res->tool_call_count = sctx.tool_count;
                for (size_t i = 0; i < sctx.tool_count; i++) {
                    res->tool_calls[i].id = sctx.tools[i].id;
                    res->tool_calls[i].name = sctx.tools[i].name;
                    res->tool_calls[i].arguments =
                        sctx.tools[i].args_buf ? sctx.tools[i].args_buf : strdup("{}");
                }
            }
        }
        free(sctx.tools);
        return 0;
    }

    /* Non-streaming response parsing */
    csilk_json_t* parsed = csilk_json_parse(resp_buf.data ? resp_buf.data : "{}");
    free(resp_buf.data);
    if (!parsed) {
        res->error_message = strdup("JSON parse error on Anthropic response");
        return -1;
    }

    csilk_json_t* content_arr = csilk_json_get_array(parsed, "content");
    if (content_arr) {
        size_t csize = csilk_json_array_size(content_arr);
        size_t total_tools = 0;
        for (size_t i = 0; i < csize; i++) {
            csilk_json_t* item = csilk_json_array_get(content_arr, i);
            if (!item) {
                continue;
            }
            const char* itype = csilk_json_get_string(item, "type");
            if (itype && strcmp(itype, "text") == 0) {
                const char* txt = csilk_json_get_string(item, "text");
                if (txt) {
                    size_t tlen = strlen(txt);
                    size_t cur = res->content ? strlen(res->content) : 0;
                    char*  nb = realloc(res->content, cur + tlen + 1);
                    if (nb) {
                        memcpy(nb + cur, txt, tlen);
                        nb[cur + tlen] = '\0';
                        res->content = nb;
                    }
                }
            } else if (itype && strcmp(itype, "thinking") == 0) {
                const char* th = csilk_json_get_string(item, "thinking");
                if (th) {
                    size_t tlen = strlen(th);
                    size_t cur = res->reasoning_content ? strlen(res->reasoning_content) : 0;
                    char*  nb = realloc(res->reasoning_content, cur + tlen + 1);
                    if (nb) {
                        memcpy(nb + cur, th, tlen);
                        nb[cur + tlen] = '\0';
                        res->reasoning_content = nb;
                    }
                }
            } else if (itype && strcmp(itype, "tool_use") == 0) {
                total_tools++;
            }
        }

        if (total_tools > 0) {
            res->tool_calls = calloc(total_tools, sizeof(csilk_ai_tool_call_t));
            if (res->tool_calls) {
                size_t tidx = 0;
                for (size_t i = 0; i < csize && tidx < total_tools; i++) {
                    csilk_json_t* item = csilk_json_array_get(content_arr, i);
                    if (!item) {
                        continue;
                    }
                    const char* itype = csilk_json_get_string(item, "type");
                    if (itype && strcmp(itype, "tool_use") == 0) {
                        const char*   id = csilk_json_get_string(item, "id");
                        const char*   name = csilk_json_get_string(item, "name");
                        csilk_json_t* inp = csilk_json_get_object(item, "input");
                        res->tool_calls[tidx].id = id ? strdup(id) : strdup("");
                        res->tool_calls[tidx].name = name ? strdup(name) : strdup("");
                        res->tool_calls[tidx].arguments =
                            inp ? csilk_json_serialize(inp, NULL) : strdup("{}");
                        tidx++;
                    }
                }
                res->tool_call_count = tidx;
            }
        }
    }

    csilk_json_t* usage = csilk_json_get_object(parsed, "usage");
    if (usage) {
        res->prompt_tokens = (int)csilk_json_get_int(usage, "input_tokens");
        res->completion_tokens = (int)csilk_json_get_int(usage, "output_tokens");
        res->total_tokens = res->prompt_tokens + res->completion_tokens;
    }

    csilk_json_free(parsed);
    return 0;
}

static csilk_ai_driver_t s_anthropic_driver = {
    .name = "anthropic",
    .init = anthropic_init,
    .free = anthropic_free,
    .chat = anthropic_chat,
    .embeddings = NULL, /* Anthropic does not have an embeddings endpoint */
};

static csilk_ai_driver_t s_claude_driver = {
    .name = "claude",
    .init = anthropic_init,
    .free = anthropic_free,
    .chat = anthropic_chat,
    .embeddings = NULL,
};

void
csilk_ai_anthropic_init_driver(void)
{
    csilk_ai_register_driver(&s_anthropic_driver);
    csilk_ai_register_driver(&s_claude_driver);
}
