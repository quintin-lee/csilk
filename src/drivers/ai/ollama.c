/**
 * @file ai_ollama.c
 * @brief Ollama driver for the AI unified interface.
 *
 * Implements the csilk_ai_driver_t vtable for local LLM inference via Ollama.
 * Key differences from the OpenAI driver:
 *   - No API key required (Ollama runs locally by default).
 *   - Chat uses POST /api/chat with a nested "options" object for parameters.
 *   - Token counts use "prompt_eval_count" / "eval_count" instead of "usage".
 *   - No streaming support yet (simplified with "stream": false).
 *
 * @copyright MIT License
 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/drivers/ai.h"
#include "csilk/csilk.h"

/** @brief Per-instance state for the Ollama driver. */
typedef struct {
    char* base_url; /**< Base URL of the Ollama server (e.g.,
                     http://localhost:11434). */
} ollama_state_t;

/**
 * @brief Initialize the Ollama driver state.
 * @note api_key is accepted but ignored -- Ollama's local server requires no
 *       authentication. Allocates state, sets the server URL (defaulting to
 *       localhost:11434), and performs one-shot libcurl global init.
 */
static void*
ollama_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    ollama_state_t* state = malloc(sizeof(ollama_state_t));
    if (!state) {
        return NULL;
    }

    state->base_url = strdup(base_url ? base_url : "http://localhost:11434");

    /* One-shot libcurl global init (idempotent across driver instances) */
    static int curl_init = 0;
    if (!curl_init) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_init = 1;
    }

    return state;
}

/**
 * @brief Free the Ollama driver state and associated resources.
 */
static void
ollama_free(void* state_ptr)
{
    ollama_state_t* state = (ollama_state_t*)state_ptr;
    if (!state) {
        return;
    }
    free(state->base_url);
    free(state);
}

/** @brief Accumulates a complete HTTP response body in memory. */
struct curl_response {
    char*  body; /**< Heap-allocated body buffer (grown on each write callback). */
    size_t size; /**< Current length of data in body (excluding NUL terminator). */
};

/**
 * @brief libcurl write callback that appends data to a growing buffer.
 * @return Number of bytes "consumed" (must equal realsize for the transfer
 *         to continue), or 0 to abort the transfer on OOM.
 */
static size_t
write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t                realsize = size * nmemb;
    struct curl_response* res = (struct curl_response*)userp;

    char* ptr = realloc(res->body, res->size + realsize + 1);
    if (!ptr) {
        return 0;
    }

    res->body = ptr;
    memcpy(&(res->body[res->size]), contents, realsize);
    res->size += realsize;
    res->body[res->size] = 0;

    return realsize;
}

/* Accumulator for streaming NDJSON lines from Ollama */
struct ollama_stream_ctx {
    const csilk_ai_chat_request_t* req;
    csilk_ai_chat_response_t*      res;
    char*                          line_buf;
    size_t                         line_size;
    size_t                         line_cap;
};

static void
ollama_parse_tool_calls(csilk_json_t* tc_arr, csilk_ai_chat_response_t* res)
{
    if (!tc_arr || !csilk_json_is_array(tc_arr)) {
        return;
    }
    size_t count = csilk_json_array_size(tc_arr);
    if (count == 0) {
        return;
    }
    csilk_ai_tool_call_t* calls = calloc(count, sizeof(csilk_ai_tool_call_t));
    if (!calls) {
        return;
    }
    size_t valid = 0;
    for (size_t i = 0; i < count; i++) {
        csilk_json_t* item = csilk_json_array_get(tc_arr, i);
        if (!item) {
            continue;
        }
        csilk_json_t* fn = csilk_json_get_object(item, "function");
        if (!fn) {
            continue;
        }
        const char*   name = csilk_json_get_string(fn, "name");
        csilk_json_t* args = csilk_json_get_object(fn, "arguments");
        char*         args_str = args ? csilk_json_serialize(args, NULL) : strdup("{}");

        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "ollama_%zu", valid + 1);
        calls[valid].id = strdup(id_buf);
        calls[valid].name = strdup(name ? name : "");
        calls[valid].arguments = args_str ? args_str : strdup("{}");
        valid++;
    }
    res->tool_calls = calls;
    res->tool_call_count = valid;
}

static void
ollama_parse_ndjson_line(struct ollama_stream_ctx* ctx, const char* json_str)
{
    csilk_json_t* js = csilk_json_parse(json_str);
    if (!js) {
        return;
    }

    csilk_json_t* msg = csilk_json_get_object(js, "message");
    if (msg) {
        const char* txt = csilk_json_get_string(msg, "content");
        if (txt && txt[0]) {
            if (ctx->req->on_chunk) {
                ctx->req->on_chunk(txt, ctx->req->user_data);
            }
            size_t clen = strlen(txt);
            size_t cur = ctx->res->content ? strlen(ctx->res->content) : 0;
            char*  nb = realloc(ctx->res->content, cur + clen + 1);
            if (nb) {
                memcpy(nb + cur, txt, clen);
                nb[cur + clen] = '\0';
                ctx->res->content = nb;
            }
        }
        csilk_json_t* tc = csilk_json_get_array(msg, "tool_calls");
        if (tc && ctx->res->tool_call_count == 0) {
            ollama_parse_tool_calls(tc, ctx->res);
        }
    }

    bool done = csilk_json_get_bool(js, "done");
    if (done) {
        ctx->res->prompt_tokens = (int)csilk_json_get_int(js, "prompt_eval_count");
        ctx->res->completion_tokens = (int)csilk_json_get_int(js, "eval_count");
        ctx->res->total_tokens = ctx->res->prompt_tokens + ctx->res->completion_tokens;
    }

    csilk_json_free(js);
}

static size_t
ollama_stream_write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t                    realsize = size * nmemb;
    struct ollama_stream_ctx* ctx = (struct ollama_stream_ctx*)userp;
    const char*               data = (const char*)contents;

    for (size_t i = 0; i < realsize; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (ctx->line_size > 0) {
                ctx->line_buf[ctx->line_size] = '\0';
                ollama_parse_ndjson_line(ctx, ctx->line_buf);
                ctx->line_size = 0;
            }
        } else {
            if (ctx->line_size + 2 > ctx->line_cap) {
                size_t ncap = ctx->line_cap == 0 ? 256 : ctx->line_cap * 2;
                char*  nb = realloc(ctx->line_buf, ncap);
                if (!nb) {
                    return 0;
                }
                ctx->line_buf = nb;
                ctx->line_cap = ncap;
            }
            ctx->line_buf[ctx->line_size++] = c;
        }
    }
    return realsize;
}

/**
 * @brief Execute a chat completion against the Ollama /api/chat endpoint.
 *
 * Flow:
 *   1. Build a JSON request body with messages array + nested "options".
 *      Ollama differs from OpenAI by placing sampling params under
 *      "options": { "temperature": ..., "top_p": ... } instead of at the root.
 *   2. POST the JSON to <base_url>/api/chat with Content-Type:
 * application/json.
 *   3. On HTTP success, parse the response extracting "message"."content"
 *      for the reply text and "prompt_eval_count" / "eval_count" for token
 *      usage.
 *   4. Returns 0 on success, -1 on transport or parse failure.
 *
 * @note Streaming is not yet implemented (always sends "stream": false).
 * @note No auth headers are set -- Ollama's local server does not require one.
 */
static int
ollama_chat(void* state_ptr, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    ollama_state_t* state = (ollama_state_t*)state_ptr;
    CURL*           curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    /* --- Step 1: Serialise the request into Ollama's JSON format --- */
    csilk_json_t* root = csilk_json_object();
    if (!root) {
        curl_easy_cleanup(curl);
        res->error_message = strdup("Failed to allocate AI request JSON");
        return -1;
    }
    if (!csilk_json_add_string(root, "model", req->model ? req->model : "llama3")) {
        csilk_json_free(root);
        curl_easy_cleanup(curl);
        res->error_message = strdup("Failed to populate AI request JSON");
        return -1;
    }

    csilk_json_t* msgs = csilk_json_array();
    if (!msgs) {
        csilk_json_free(root);
        curl_easy_cleanup(curl);
        res->error_message = strdup("Failed to allocate AI messages JSON");
        return -1;
    }
    for (size_t i = 0; i < req->message_count; i++) {
        csilk_json_t* m = csilk_json_object();
        csilk_json_add_string(m, "role", req->messages[i].role ? req->messages[i].role : "user");
        /* Ollama does not support tool_calls in messages; only serialize content */
        csilk_json_add_string(
            m, "content", req->messages[i].content ? req->messages[i].content : "");
        csilk_json_array_append(msgs, m);
    }
    csilk_json_add_object(root, "messages", msgs);

    csilk_json_add_bool(root, "stream", req->stream ? true : false);

    /* Serialize tool definitions for Ollama when present */
    if (req->tools && req->tool_count > 0) {
        csilk_json_t* tools_arr = csilk_json_array();
        for (size_t i = 0; i < req->tool_count; i++) {
            csilk_json_t* t = csilk_json_object();
            csilk_json_add_string(t, "type", "function");
            csilk_json_t* fn = csilk_json_object();
            csilk_json_add_string(fn, "name", req->tools[i].function.name);
            if (req->tools[i].function.description) {
                csilk_json_add_string(fn, "description", req->tools[i].function.description);
            }
            if (req->tools[i].function.parameters_json) {
                csilk_json_t* pval = (csilk_json_t*)req->tools[i].function.parameters_json;
                if (csilk_json_is_object(pval)) {
                    csilk_json_add_object(fn, "parameters", csilk_json_copy(pval));
                } else if (csilk_json_is_string(pval)) {
                    const char*   s = csilk_json_string_value(pval);
                    csilk_json_t* pobj = s ? csilk_json_parse(s) : NULL;
                    csilk_json_add_object(fn, "parameters", pobj ? pobj : csilk_json_object());
                } else {
                    csilk_json_t* pobj = csilk_json_parse((const char*)pval);
                    csilk_json_add_object(fn, "parameters", pobj ? pobj : csilk_json_object());
                }
            } else {
                csilk_json_add_object(fn, "parameters", csilk_json_object());
            }
            csilk_json_add_object(t, "function", fn);
            csilk_json_array_append(tools_arr, t);
        }
        csilk_json_add_object(root, "tools", tools_arr);
    }

    /* Ollama wraps sampling parameters in a nested "options" object, unlike
   * OpenAI which places them at the JSON root. */
    csilk_json_t* opts = csilk_json_object();
    if (req->temperature > 0) {
        csilk_json_add_number(opts, "temperature", req->temperature);
    }
    if (req->top_p > 0) {
        csilk_json_add_number(opts, "top_p", req->top_p);
    }
    csilk_json_add_object(root, "options", opts);

    char* json_body = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    if (!json_body) {
        curl_easy_cleanup(curl);
        res->error_message = strdup("Failed to serialize AI request JSON");
        return -1;
    }

    /* Validate base_url scheme to prevent SSRF (CWE-918) */
    if (strncmp(state->base_url, "http://", 7) != 0 &&
        strncmp(state->base_url, "https://", 8) != 0) {
        CSILK_LOG_E("AI Ollama: Invalid URL scheme: %.50s...", state->base_url);
        curl_easy_cleanup(curl);
        return -1;
    }

    /* --- Step 2: Perform the HTTP POST --- */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/chat", state->base_url);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    struct curl_response     cr = {0};
    struct ollama_stream_ctx sctx = {
        .req = req,
        .res = res,
        .line_buf = NULL,
        .line_size = 0,
    };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (req->stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ollama_stream_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&sctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);
    }
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    /* Bounded time-outs so a dead/unreachable Ollama host cannot wedge the
     * request indefinitely (same guard as the OpenAI driver). */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10L * 1000L);
    if (req->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
    } else if (!req->stream) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L * 1000L);
    }

    CURLcode rc = curl_easy_perform(curl);
    free(json_body);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        res->error_message = strdup(curl_easy_strerror(rc));
        free(cr.body);
        free(sctx.line_buf);
        curl_easy_cleanup(curl);
        return -1;
    }

    if (req->stream) {
        free(sctx.line_buf);
        curl_easy_cleanup(curl);
        return (res->content || res->tool_call_count > 0) ? 0 : -1;
    }

    /* --- Step 3: Parse the Ollama response (non-streaming) --- */
    csilk_json_t* resp_root = csilk_json_parse(cr.body ? cr.body : "{}");
    if (resp_root) {
        csilk_json_t* msg = csilk_json_get(resp_root, "message");
        csilk_json_t* content = csilk_json_get(msg, "content");
        if (csilk_json_is_string(content)) {
            res->content = strdup(csilk_json_string_value(content));
        }
        csilk_json_t* tc = csilk_json_get_array(msg, "tool_calls");
        if (tc) {
            ollama_parse_tool_calls(tc, res);
        }
        /* Ollama uses different field names than OpenAI for token accounting */
        res->prompt_tokens = csilk_json_get_int(resp_root, "prompt_eval_count");
        res->completion_tokens = csilk_json_get_int(resp_root, "eval_count");
        res->total_tokens = res->prompt_tokens + res->completion_tokens;
        csilk_json_free(resp_root);
    }

    free(cr.body);
    curl_easy_cleanup(curl);
    return (res->content || res->tool_call_count > 0) ? 0 : -1;
}

/**
 * @brief Generate embeddings via Ollama's /api/embeddings endpoint.
 *
 * Ollama only accepts a single input string per request (no batching).
 * For multi-input calls, each input is sent individually and results
 * are concatenated into the response.
 *
 * See: https://github.com/ollama/ollama/blob/main/docs/api.md#generate-embeddings
 */
static int
ollama_embeddings(void*                           state_ptr,
                  const char*                     model,
                  const char**                    input,
                  size_t                          count,
                  csilk_ai_embeddings_response_t* res)
{
    ollama_state_t* state = (ollama_state_t*)state_ptr;
    if (!state || !model || !input || count == 0 || !res) {
        return -1;
    }

    res->values = NULL;
    res->count = 0;
    res->dimension = 0;

    for (size_t i = 0; i < count; i++) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return -1;
        }

        csilk_json_t* root = csilk_json_object();
        csilk_json_add_string(root, "model", model);
        csilk_json_add_string(root, "prompt", input[i]);
        char* json_body = csilk_json_serialize(root, NULL);
        csilk_json_free(root);

        char url[512];
        snprintf(url, sizeof(url), "%s/api/embeddings", state->base_url);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        struct curl_response cr = {0};
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10L * 1000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L * 1000L);

        CURLcode rc = curl_easy_perform(curl);
        free(json_body);
        curl_slist_free_all(headers);

        if (rc != CURLE_OK) {
            char err[256];
            snprintf(err, sizeof(err), "Ollama CURL error: %s", curl_easy_strerror(rc));
            res->error_message = strdup(err);
            free(cr.body);
            curl_easy_cleanup(curl);
            return -1;
        }

        csilk_json_t* resp = csilk_json_parse(cr.body);
        free(cr.body);
        if (!resp) {
            res->error_message = strdup("JSON parse error");
            curl_easy_cleanup(curl);
            return -1;
        }

        csilk_json_t* embedding = csilk_json_get(resp, "embedding");
        size_t        dim = 0;
        if (csilk_json_is_array(embedding)) {
            dim = csilk_json_array_size(embedding);
            if (res->dimension == 0) {
                res->dimension = dim;
            }
            size_t new_count = res->count + 1;
            float* new_values = realloc(res->values, sizeof(float) * new_count * res->dimension);
            if (!new_values) {
                csilk_json_free(resp);
                curl_easy_cleanup(curl);
                return -1;
            }
            res->values = new_values;
            for (size_t j = 0; j < dim; j++) {
                res->values[res->count * res->dimension + j] =
                    (float)csilk_json_number_value(csilk_json_array_get(embedding, j));
            }
            res->count = (int)new_count;
        }

        csilk_json_free(resp);
        curl_easy_cleanup(curl);
    }

    return 0;
}

/** @brief Driver vtable for the Ollama AI backend. */
static const csilk_ai_driver_t ollama_driver = {
    .name = "ollama",
    .init = ollama_init,
    .chat = ollama_chat,
    .embeddings = ollama_embeddings,
    .free = ollama_free,
};

/**
 * @brief Register the Ollama driver with the AI subsystem.
 * Called during startup to make "ollama" available to csilk_ai_new().
 */
void
csilk_ai_ollama_init_driver(void)
{
    csilk_ai_register_driver(&ollama_driver);
}
