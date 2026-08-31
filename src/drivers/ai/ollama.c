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
    csilk_json_add_string(root, "model", req->model ? req->model : "llama3");

    csilk_json_t* msgs = csilk_json_array();
    for (size_t i = 0; i < req->message_count; i++) {
        csilk_json_t* m = csilk_json_object();
        csilk_json_add_string(m, "role", req->messages[i].role ? req->messages[i].role : "user");
        /* Ollama does not support tool_calls in messages; only serialize content */
        csilk_json_add_string(
            m, "content", req->messages[i].content ? req->messages[i].content : "");
        csilk_json_array_append(msgs, m);
    }
    csilk_json_add_object(root, "messages", msgs);

    /* Streaming disabled for simplicity; the response body contains the
   * complete reply rather than SSE chunks. */
    csilk_json_add_bool(root, "stream", false);

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

    struct curl_response cr = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);

    CURLcode rc = curl_easy_perform(curl);
    free(json_body);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        res->error_message = strdup(curl_easy_strerror(rc));
        free(cr.body);
        curl_easy_cleanup(curl);
        return -1;
    }

    /* --- Step 3: Parse the Ollama response --- */
    csilk_json_t* resp_root = csilk_json_parse(cr.body);
    if (resp_root) {
        csilk_json_t* msg = csilk_json_get(resp_root, "message");
        csilk_json_t* content = csilk_json_get(msg, "content");
        if (csilk_json_is_string(content)) {
            res->content = strdup(csilk_json_string_value(content));
        }
        /* Ollama uses different field names than OpenAI for token accounting */
        res->prompt_tokens = csilk_json_get_int(resp_root, "prompt_eval_count");
        res->completion_tokens = csilk_json_get_int(resp_root, "eval_count");
        res->total_tokens = res->prompt_tokens + res->completion_tokens;
        csilk_json_free(resp_root);
    }

    free(cr.body);
    curl_easy_cleanup(curl);
    return res->content ? 0 : -1;
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
