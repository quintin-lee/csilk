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

#include "cJSON.h"
#include "csilk/drivers/ai.h"

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
		return nullptr;
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
	char* body;  /**< Heap-allocated body buffer (grown on each write callback). */
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
	size_t realsize = size * nmemb;
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
	CURL* curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	/* --- Step 1: Serialise the request into Ollama's JSON format --- */
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", req->model ? req->model : "llama3");

	cJSON* msgs = cJSON_CreateArray();
	for (size_t i = 0; i < req->message_count; i++) {
		cJSON* m = cJSON_CreateObject();
		cJSON_AddStringToObject(m, "role", req->messages[i].role);
		cJSON_AddStringToObject(m, "content", req->messages[i].content);
		cJSON_AddItemToArray(msgs, m);
	}
	cJSON_AddItemToObject(root, "messages", msgs);

	/* Streaming disabled for simplicity; the response body contains the
   * complete reply rather than SSE chunks. */
	cJSON_AddBoolToObject(root, "stream", false);

	/* Ollama wraps sampling parameters in a nested "options" object, unlike
   * OpenAI which places them at the JSON root. */
	cJSON* opts = cJSON_CreateObject();
	if (req->temperature > 0) {
		cJSON_AddNumberToObject(opts, "temperature", req->temperature);
	}
	if (req->top_p > 0) {
		cJSON_AddNumberToObject(opts, "top_p", req->top_p);
	}
	cJSON_AddItemToObject(root, "options", opts);

	char* json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	/* --- Step 2: Perform the HTTP POST --- */
	char url[512];
	snprintf(url, sizeof(url), "%s/api/chat", state->base_url);

	struct curl_slist* headers = nullptr;
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
		res->error_message = strdup(curl_easy_strerror(rc));
		free(cr.body);
		curl_easy_cleanup(curl);
		return -1;
	}

	/* --- Step 3: Parse the Ollama response --- */
	cJSON* resp_root = cJSON_Parse(cr.body);
	if (resp_root) {
		cJSON* msg = cJSON_GetObjectItem(resp_root, "message");
		cJSON* content = cJSON_GetObjectItem(msg, "content");
		if (cJSON_IsString(content)) {
			res->content = strdup(content->valuestring);
		}
		/* Ollama uses different field names than OpenAI for token accounting */
		res->prompt_tokens = cJSON_GetObjectItem(resp_root, "prompt_eval_count")->valueint;
		res->completion_tokens = cJSON_GetObjectItem(resp_root, "eval_count")->valueint;
		res->total_tokens = res->prompt_tokens + res->completion_tokens;
		cJSON_Delete(resp_root);
	}

	free(cr.body);
	curl_easy_cleanup(curl);
	return res->content ? 0 : -1;
}

/**
 * @brief Generate embeddings via Ollama (stub -- not yet implemented).
 * @todo Implement using Ollama's /api/embeddings endpoint.
 */
static int
ollama_embeddings(void* state_ptr,
		  const char* model,
		  const char** input,
		  size_t count,
		  csilk_ai_embeddings_response_t* res)
{
	(void)state_ptr;
	(void)model;
	(void)input;
	(void)count;
	(void)res;
	return -1;
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
