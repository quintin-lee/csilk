/**
 * @file ai.c
 * @brief AI unified interface engine implementation.
 *
 * Architecture: Facade pattern over pluggable AI driver backends (OpenAI,
 * Ollama, etc.). The global driver registry is populated once at first use
 * via lazy initialization. Each csilk_ai_t instance wraps a single driver
 * with its private state.
 *
 * The module provides both synchronous (blocking) and asynchronous (libuv
 * thread-pool) variants for chat completions and embeddings. Chat requests
 * include automatic retry with exponential backoff for transient failures
 * (network errors, rate limits, server errors).
 *
 * A context helper manages sliding-window conversation history for multi-turn
 * interactions. Memory for async operations is heap-allocated and freed in
 * the after-work callback on the main loop thread.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/ai.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "cJSON.h"
#include "csilk/csilk.h"

/* --- Global AI Metrics --- */
static atomic_uint_fast64_t ai_requests_total = 0;
static atomic_uint_fast64_t ai_tokens_total = 0;
static atomic_uint_fast64_t ai_prompt_tokens = 0;
static atomic_uint_fast64_t ai_completion_tokens = 0;
static atomic_uint_fast64_t ai_errors_total = 0;
static atomic_uint_fast64_t ai_duration_us_total = 0;

/* --- AI Monitor Registry --- */
static csilk_ctx_t* g_ai_monitors[16];
static size_t g_ai_monitor_count = 0;
static uv_mutex_t g_ai_monitor_mutex;
static atomic_int g_ai_monitor_init = 0;

static void
ai_ensure_monitor_init(void)
{
	int expected = 0;
	if (atomic_compare_exchange_strong(&g_ai_monitor_init, &expected, 1)) {
		uv_mutex_init(&g_ai_monitor_mutex);
	}
}

static void
_ai_broadcast(const char* event,
	      const char* model,
	      int status,
	      int prompt_tokens,
	      int completion_tokens,
	      uint64_t duration_us,
	      const char* error)
{
	ai_ensure_monitor_init();
	if (g_ai_monitor_count == 0) {
		return;
	}

	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (model) {
		cJSON_AddStringToObject(root, "model", model);
	}
	cJSON_AddNumberToObject(root, "status", status);
	cJSON_AddNumberToObject(root, "prompt_tokens", (double)prompt_tokens);
	cJSON_AddNumberToObject(root, "completion_tokens", (double)completion_tokens);
	cJSON_AddNumberToObject(root, "duration_ms", (double)duration_us / 1000.0);
	if (error) {
		cJSON_AddStringToObject(root, "error", error);
	}
	cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));

	char* json = cJSON_PrintUnformatted(root);
	uv_mutex_lock(&g_ai_monitor_mutex);
	for (size_t i = 0; i < g_ai_monitor_count; i++) {
		csilk_ws_send(g_ai_monitors[i], (uint8_t*)json, strlen(json), 0x1);
	}
	uv_mutex_unlock(&g_ai_monitor_mutex);
	free(json);
	cJSON_Delete(root);
}

void
csilk_ai_get_stats(csilk_ai_stats_t* stats)
{
	if (!stats) {
		return;
	}
	stats->requests_total = atomic_load(&ai_requests_total);
	stats->tokens_total = atomic_load(&ai_tokens_total);
	stats->prompt_tokens = atomic_load(&ai_prompt_tokens);
	stats->completion_tokens = atomic_load(&ai_completion_tokens);
	stats->errors_total = atomic_load(&ai_errors_total);
	stats->duration_us_total = atomic_load(&ai_duration_us_total);
}

char*
csilk_ai_stats_to_json(const csilk_ai_stats_t* stats)
{
	if (!stats) {
		return nullptr;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "requests_total", (double)stats->requests_total);
	cJSON_AddNumberToObject(root, "tokens_total", (double)stats->tokens_total);
	cJSON_AddNumberToObject(root, "prompt_tokens", (double)stats->prompt_tokens);
	cJSON_AddNumberToObject(root, "completion_tokens", (double)stats->completion_tokens);
	cJSON_AddNumberToObject(root, "errors_total", (double)stats->errors_total);
	cJSON_AddNumberToObject(root, "duration_us_total", (double)stats->duration_us_total);
	char* json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

void
csilk_ai_register_monitor(void* c)
{
	ai_ensure_monitor_init();
	uv_mutex_lock(&g_ai_monitor_mutex);
	if (g_ai_monitor_count < 16) {
		g_ai_monitors[g_ai_monitor_count++] = (csilk_ctx_t*)c;
		CSILK_LOG_I("Registered AI monitor: %p", c);
	} else {
		CSILK_LOG_E("Failed to register AI monitor: monitor registry is full");
	}
	uv_mutex_unlock(&g_ai_monitor_mutex);
}

/** @brief Global registry of AI driver implementations.
 *  Populated once during the first csilk_ai_new() call via lazy init of
 *  the built-in drivers (OpenAI, Ollama). Not thread-safe for concurrent
 *  registration — all registration happens during the single-threaded
 *  startup phase. */
static const csilk_ai_driver_t* g_drivers[MAX_DRIVERS];
static size_t g_driver_count = 0;

/** @brief Opaper AI engine handle wrapping a single driver backend.
 *  Each instance pairs a driver vtable with its private initialization
 *  state (API credentials, connection pool, etc.). Created via
 *  csilk_ai_new() and freed via csilk_ai_free(). Not thread-safe for
 *  concurrent use from multiple threads. */
struct csilk_ai_s {
	const csilk_ai_driver_t* driver; /**< Driver vtable (init, chat, embeddings, free). */
	void* driver_state;		 /**< Driver-private state (API key, base URL, etc.). */
};

/** @brief Register an AI driver implementation in the global registry.
 *  Called during driver module initialization (e.g.,
 * csilk_ai_openai_init_driver()). Silently ignores registration if the registry
 * is full.
 *  @param driver Driver vtable with name, init, chat, embeddings, free. */
void
csilk_ai_register_driver(const csilk_ai_driver_t* driver)
{
	if (g_driver_count < MAX_DRIVERS) {
		g_drivers[g_driver_count++] = driver;
		CSILK_LOG_I("Registered AI driver: '%s'", driver->name);
	} else {
		CSILK_LOG_E("Failed to register AI driver '%s': registry is full", driver->name);
	}
}

/** @brief Linear search of the global driver registry by name.
 *  @param name Driver name (e.g., "openai", "ollama").
 *  @return Pointer to the driver vtable, or nullptr if not found. */
static const csilk_ai_driver_t*
find_driver(const char* name)
{
	for (size_t i = 0; i < g_driver_count; i++) {
		if (strcmp(g_drivers[i]->name, name) == 0) {
			return g_drivers[i];
		}
	}
	return nullptr;
}

/* Forward declarations for default drivers */
extern void csilk_ai_openai_init_driver(void);
extern void csilk_ai_ollama_init_driver(void);

/** @brief Create a new AI engine instance bound to a specific driver backend.
 *
 *  On the very first call, lazy-initializes the built-in driver registry
 *  (OpenAI and Ollama). Subsequent calls reuse the already-registered drivers.
 *  If the named driver is not found or its init() fails, returns nullptr.
 *
 *  @param driver_name Backend name (e.g., "openai", "ollama").
 *  @param api_key     API key for authentication (e.g., OpenAI API key).
 *  @param base_url    Optional custom base URL (nullptr for driver default).
 *  @return Newly allocated csilk_ai_t, or nullptr on failure.
 *  @note The caller owns the returned handle and must free with
 * csilk_ai_free(). On allocation failure, the driver state is freed internally
 * to avoid leaks. */
csilk_ai_t*
csilk_ai_new(const char* driver_name, const char* api_key, const char* base_url)
{
	/* Lazy init: register default drivers only on first call to avoid
     pulling in unused driver dependencies at process startup. */
	static int initialized = 0;
	if (!initialized) {
		CSILK_LOG_D("Lazy-initializing default AI drivers (OpenAI, Ollama)...");
		csilk_ai_openai_init_driver();
		csilk_ai_ollama_init_driver();
		initialized = 1;
	}

	const csilk_ai_driver_t* driver = find_driver(driver_name);
	if (!driver) {
		CSILK_LOG_E("Failed to create AI instance: driver '%s' not found", driver_name);
		return nullptr;
	}

	void* state = driver->init(api_key, base_url);
	if (!state) {
		CSILK_LOG_E("Failed to initialize AI driver '%s' state (base_url: %s)",
			    driver_name,
			    base_url ? base_url : "default");
		return nullptr;
	}

	csilk_ai_t* ai = malloc(sizeof(csilk_ai_t));
	if (!ai) {
		CSILK_LOG_E("Failed to allocate memory for AI instance (driver: '%s')",
			    driver_name);
		driver->free(state);
		return nullptr;
	}

	ai->driver = driver;
	ai->driver_state = state;
	CSILK_LOG_I("AI instance successfully created using driver '%s' (base_url: %s)",
		    driver_name,
		    base_url ? base_url : "default");
	return ai;
}

/** @brief Send a chat completion request with automatic retry on transient
 *  failures.
 *
 *  Algorithm:
 *  1. Zero out the response struct for each attempt.
 *  2. Call the driver's chat() implementation.
 *  3. On success (status == 0), return immediately.
 *  4. On failure, check the error message for known transient error patterns
 *     (CURL transport errors, HTTP 429 rate limit, 502/503 server errors).
 *  5. If retryable and attempts remain, sleep with exponential backoff
 *     (1s, 2s) and retry. Non-retryable errors break immediately.
 *
 *  @param ai  AI engine handle (must not be nullptr).
 *  @param req Chat request parameters (model, messages, temperature, tools).
 *  @param res [out] Receives the chat response, including content, tool calls,
 *              and token usage. Zeroed on each retry attempt.
 *  @return 0 on success, -1 if all retries are exhausted or parameters invalid.
 *  @note Not thread-safe. The caller must serialize access to the same
 *        csilk_ai_t handle. */
int
csilk_ai_chat(csilk_ai_t* ai, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
	if (!ai || !req || !res) {
		CSILK_LOG_E("Invalid arguments provided to csilk_ai_chat");
		return -1;
	}

	uint64_t start = uv_hrtime();
	atomic_fetch_add(&ai_requests_total, 1);

	CSILK_LOG_D("AI chat request initiated. Model: '%s', Messages count: %zu, Tools count: %zu",
		    req->model ? req->model : "default",
		    req->message_count,
		    req->tool_count);

	int retries = 0;
	int max_retries = 2;
	int status = -1;

	while (retries <= max_retries) {
		memset(res, 0, sizeof(*res));
		status = ai->driver->chat(ai->driver_state, req, res);

		if (status == 0) {
			break;
		}

		/* Retry only on transient errors: network issues (CURL),
       rate limits (429), or temporary server unavailability (502/503). */
		if (res->error_message &&
		    (strstr(res->error_message, "CURL error") ||
		     strstr(res->error_message, "429") || strstr(res->error_message, "502") ||
		     strstr(res->error_message, "503"))) {
			retries++;
			if (retries <= max_retries) {
				/* Exponential backoff: 1s, then 2s. This avoids thundering herd
           on rate-limited endpoints. */
				unsigned int wait_ms = (unsigned int)(1000 * (1 << (retries - 1)));
				CSILK_LOG_W("AI chat transient failure: '%s'. Retrying in %u ms "
					    "(attempt %d/%d)...",
					    res->error_message,
					    wait_ms,
					    retries,
					    max_retries);
				uv_sleep(wait_ms);
				csilk_ai_chat_response_free(res);
				continue;
			}
		}
		break;
	}

	uint64_t duration = (uv_hrtime() - start) / 1000; // us
	atomic_fetch_add(&ai_duration_us_total, duration);

	if (status == 0) {
		atomic_fetch_add(&ai_tokens_total, res->total_tokens);
		atomic_fetch_add(&ai_prompt_tokens, res->prompt_tokens);
		atomic_fetch_add(&ai_completion_tokens, res->completion_tokens);
		CSILK_LOG_I("AI chat request succeeded. Model: '%s', Prompt tokens: %d, Completion "
			    "tokens: %d, Total tokens: %d, Duration: %.2f ms",
			    req->model ? req->model : "default",
			    res->prompt_tokens,
			    res->completion_tokens,
			    res->total_tokens,
			    (double)duration / 1000.0);
	} else {
		atomic_fetch_add(&ai_errors_total, 1);
		CSILK_LOG_E("AI chat request failed after %d attempts. Model: '%s', Error: %s",
			    retries + 1,
			    req->model ? req->model : "default",
			    res->error_message ? res->error_message : "Unknown error");
	}

	_ai_broadcast("ai_chat",
		      req->model,
		      status,
		      res->prompt_tokens,
		      res->completion_tokens,
		      duration,
		      res->error_message);

	return status;
}

/** @brief Per-async-chat-request context passed between the work callback
 *  (on a thread-pool thread) and the after-work callback (on the main loop
 *  thread). Keeps the response on the heap so it survives across threads
 *  without data races on the caller's stack. */
typedef struct {
	csilk_ai_t* ai;			    /**< AI engine handle. */
	const csilk_ai_chat_request_t* req; /**< Request parameters (caller-owned). */
	csilk_ai_chat_response_t res;	    /**< Response buffer (filled by worker). */
	csilk_ai_chat_async_cb cb;	    /**< Completion callback. */
	void* user_data;		    /**< Opaque user context for callback. */
	int status;			    /**< Result code from csilk_ai_chat(). */
} async_chat_req_t;

/** @brief libuv thread-pool work callback — runs csilk_ai_chat() off the main
 *  loop thread. The response is stored in the heap-allocated async context. */
static void
chat_work_cb(uv_work_t* req)
{
	async_chat_req_t* ar = (async_chat_req_t*)req->data;
	ar->status = csilk_ai_chat(ar->ai, ar->req, &ar->res);
}

/** @brief libuv after-work callback — delivers the result on the main loop
 *  thread via the user's callback, then frees the async context. */
static void
chat_after_work_cb(uv_work_t* req, int status)
{
	(void)status;
	async_chat_req_t* ar = (async_chat_req_t*)req->data;
	ar->cb(ar->status, &ar->res, ar->user_data);
	free(ar);
	free(req);
}

/** @brief Send a chat completion request asynchronously on the libuv thread
 *  pool.
 *
 *  Allocates a work request and an async context on the heap, queues the
 *  work via uv_queue_work(), and returns immediately. The callback fires
 *  on the main loop thread after the driver's chat() completes. The response
 *  is valid only during the callback invocation.
 *
 *  Ownership: The caller retains ownership of @p req. The response @p res
 *  is owned by the async context and is freed after the callback returns.
 *  If the caller needs the response beyond the callback, it must deep-copy it.
 *
 *  @param ai        AI engine handle.
 *  @param req       Chat request (must remain valid until the callback fires).
 *  @param cb        Completion callback (required).
 *  @param user_data Opaque pointer passed through to the callback.
 *  @note Thread-safe to call from any thread (libuv queues the work).
 *        On allocation failure, this is a silent no-op. */
void
csilk_ai_chat_async(csilk_ai_t* ai,
		    const csilk_ai_chat_request_t* req,
		    csilk_ai_chat_async_cb cb,
		    void* user_data)
{
	if (!ai || !req || !cb) {
		CSILK_LOG_E("Invalid arguments provided to csilk_ai_chat_async");
		return;
	}

	uv_work_t* work = malloc(sizeof(uv_work_t));
	async_chat_req_t* ar = malloc(sizeof(async_chat_req_t));
	if (!work || !ar) {
		CSILK_LOG_E("Failed to allocate memory for async chat request");
		free(work);
		free(ar);
		return;
	}

	ar->ai = ai;
	ar->req = req;
	ar->cb = cb;
	ar->user_data = user_data;
	memset(&ar->res, 0, sizeof(ar->res));

	work->data = ar;
	CSILK_LOG_D("Queueing async chat request for model '%s'",
		    req->model ? req->model : "default");
	uv_queue_work(uv_default_loop(), work, chat_work_cb, chat_after_work_cb);
}

/** @brief Generate embeddings for a batch of input strings.
 *
 * Checks that the driver supports embeddings (optional operation),
 * then delegates to the driver's embeddings() implementation.
 *
 * @param ai    AI engine handle.
 * @param model Model name (e.g., "text-embedding-3-small").
 * @param input Array of input strings to embed.
 * @param count Number of input strings.
 * @param res   [out] Receives the embeddings values and error message.
 * @return 0 on success, -1 if the driver lacks embeddings support or
 *         parameters are invalid.
 * @note Not thread-safe on the same csilk_ai_t handle. */
int
csilk_ai_embeddings(csilk_ai_t* ai,
		    const char* model,
		    const char** input,
		    size_t count,
		    csilk_ai_embeddings_response_t* res)
{
	if (!ai || !model || !input || !res) {
		CSILK_LOG_E("Invalid arguments provided to csilk_ai_embeddings");
		return -1;
	}

	uint64_t start = uv_hrtime();
	atomic_fetch_add(&ai_requests_total, 1);

	CSILK_LOG_D(
	    "AI embeddings request initiated. Model: '%s', Inputs count: %zu", model, count);

	memset(res, 0, sizeof(*res));
	if (!ai->driver->embeddings) {
		res->error_message = strdup("Driver does not support embeddings");
		atomic_fetch_add(&ai_errors_total, 1);
		CSILK_LOG_E(
		    "Failed to generate AI embeddings: driver '%s' does not support embeddings",
		    ai->driver->name);
		_ai_broadcast("ai_embeddings", model, -1, 0, 0, 0, res->error_message);
		return -1;
	}

	int status = ai->driver->embeddings(ai->driver_state, model, input, count, res);

	uint64_t duration = (uv_hrtime() - start) / 1000;
	atomic_fetch_add(&ai_duration_us_total, duration);

	if (status == 0) {
		atomic_fetch_add(&ai_tokens_total, res->total_tokens);
		atomic_fetch_add(&ai_prompt_tokens, res->prompt_tokens);
		CSILK_LOG_I("AI embeddings generated successfully. Model: '%s', Prompt tokens: %d, "
			    "Total tokens: %d, Duration: %.2f ms",
			    model,
			    res->prompt_tokens,
			    res->total_tokens,
			    (double)duration / 1000.0);
	} else {
		atomic_fetch_add(&ai_errors_total, 1);
		CSILK_LOG_E("Failed to generate AI embeddings. Model: '%s', Error: %s",
			    model,
			    res->error_message ? res->error_message : "Unknown error");
	}

	_ai_broadcast(
	    "ai_embeddings", model, status, res->prompt_tokens, 0, duration, res->error_message);

	return status;
}

/** @brief Per-async-embedding-request context passed between the work
 *  callback (thread-pool thread) and after-work callback (main loop). */
typedef struct {
	csilk_ai_t* ai;
	const char* model;
	const char** input;
	size_t count;
	csilk_ai_embeddings_response_t res;
	csilk_ai_embeddings_async_cb cb;
	void* user_data;
	int status;
} async_emb_req_t;

/** @brief libuv thread-pool work callback for async embeddings.
 *  Runs csilk_ai_embeddings() off the main loop thread, storing
 *  the result in the heap-allocated async context. */
static void
emb_work_cb(uv_work_t* req)
{
	async_emb_req_t* ar = (async_emb_req_t*)req->data;
	ar->status = csilk_ai_embeddings(ar->ai, ar->model, ar->input, ar->count, &ar->res);
}

/** @brief libuv after-work callback for async embeddings — delivers the
 *  result on the main loop thread via the user's callback, then frees
 *  the async context. */
static void
emb_after_work_cb(uv_work_t* req, int status)
{
	(void)status;
	async_emb_req_t* ar = (async_emb_req_t*)req->data;
	ar->cb(ar->status, &ar->res, ar->user_data);
	free(ar);
	free(req);
}

/** @brief Generate embeddings asynchronously on the libuv thread pool.
 *
 * Allocates a work request and async context on the heap, queues
 * via uv_queue_work(), and returns immediately. The callback fires
 * on the main loop thread after completion.
 *
 * @param ai         AI engine handle.
 * @param model      Model name.
 * @param input      Array of input strings (must remain valid until callback).
 * @param count      Number of input strings.
 * @param cb         Completion callback (required).
 * @param user_data  Opaque pointer passed through to callback.
 * @note The response is valid only during the callback invocation.
 *       Thread-safe to call from any thread. */
void
csilk_ai_embeddings_async(csilk_ai_t* ai,
			  const char* model,
			  const char** input,
			  size_t count,
			  csilk_ai_embeddings_async_cb cb,
			  void* user_data)
{
	if (!ai || !model || !input || !cb) {
		CSILK_LOG_E("Invalid arguments provided to csilk_ai_embeddings_async");
		return;
	}

	uv_work_t* work = malloc(sizeof(uv_work_t));
	async_emb_req_t* ar = malloc(sizeof(async_emb_req_t));
	if (!work || !ar) {
		CSILK_LOG_E("Failed to allocate memory for async embeddings request");
		free(work);
		free(ar);
		return;
	}

	ar->ai = ai;
	ar->model = model;
	ar->input = input;
	ar->count = count;
	ar->cb = cb;
	ar->user_data = user_data;
	memset(&ar->res, 0, sizeof(ar->res));

	work->data = ar;
	CSILK_LOG_D("Queueing async embeddings request for model '%s' (inputs: %zu)", model, count);
	uv_queue_work(uv_default_loop(), work, emb_work_cb, emb_after_work_cb);
}

/** @brief Free an AI engine handle and its driver state.
 *  Calls the driver's free() callback first, then frees the handle.
 *  @param ai The handle to free (may be nullptr). */
void
csilk_ai_free(csilk_ai_t* ai)
{
	if (!ai) {
		return;
	}
	CSILK_LOG_D("Freeing AI engine instance and driver state for driver '%s'",
		    ai->driver->name);
	ai->driver->free(ai->driver_state);
	free(ai);
}

/** @brief Free all dynamically allocated fields in a chat response.
 *  Frees content, tool calls (with their id/name/arguments), raw_response,
 *  and error_message. Does NOT free the struct itself.
 *  @param res Response struct to clean (may be nullptr). Safe to call on
 *         a zero-initialized struct. */
void
csilk_ai_chat_response_free(csilk_ai_chat_response_t* res)
{
	if (!res) {
		return;
	}
	free(res->content);
	if (res->tool_calls) {
		for (size_t i = 0; i < res->tool_call_count; i++) {
			free(res->tool_calls[i].id);
			free(res->tool_calls[i].name);
			free(res->tool_calls[i].arguments);
		}
		free(res->tool_calls);
	}
	free(res->raw_response);
	free(res->error_message);
}

/** @brief Free dynamically allocated fields in an embeddings response.
 *  Frees values array and error_message. Does NOT free the struct itself.
 *  @param res Response struct to clean (may be nullptr). */
void
csilk_ai_embeddings_response_free(csilk_ai_embeddings_response_t* res)
{
	if (!res) {
		return;
	}
	free(res->values);
	free(res->error_message);
}

/* --- Context Helpers Implementation --- */

/** @brief Create a new conversation context with sliding-window history.
 *
 * Allocates a context struct that manages a rolling window of
 * message history. When max_history messages are reached, the
 * oldest message is evicted on each new add().
 *
 * @param max_history Maximum number of messages to retain (0 = unlimited).
 * @return A new context handle, or nullptr on allocation failure.
 * @note The caller must free the handle with csilk_ai_context_free(). */
csilk_ai_context_t*
csilk_ai_context_new(size_t max_history)
{
	csilk_ai_context_t* ctx = calloc(1, sizeof(csilk_ai_context_t));
	if (ctx) {
		ctx->max_history = max_history;
		CSILK_LOG_D("Created new AI context (max_history: %zu)", max_history);
	} else {
		CSILK_LOG_E("Failed to allocate memory for new AI context");
	}
	return ctx;
}

/** @brief Add a message to the conversation context with sliding-window
 * eviction.
 *
 * Algorithm:
 * 1. If the internal message array is full, double its capacity.
 * 2. If max_history > 0 and count >= max_history, remove the oldest
 *    message (free its role and content strings, shift remaining messages
 *    left via memmove, decrement count).
 * 3. strdup the role and content, append to the message array.
 *
 * @param ctx     Conversation context.
 * @param role    Message role (e.g., "user", "assistant", "system").
 * @param content Message content text.
 * @note The role and content strings are deep-copied internally. */
void
csilk_ai_context_add(csilk_ai_context_t* ctx, const char* role, const char* content)
{
	if (!ctx || !role || !content) {
		CSILK_LOG_E("Invalid arguments provided to csilk_ai_context_add");
		return;
	}

	if (ctx->count >= ctx->capacity) {
		size_t new_cap = ctx->capacity == 0 ? 8 : ctx->capacity * 2;
		csilk_ai_message_t* new_msgs =
		    realloc(ctx->messages, sizeof(csilk_ai_message_t) * new_cap);
		if (!new_msgs) {
			CSILK_LOG_E(
			    "Failed to allocate memory to expand AI context messages array");
			return;
		}
		ctx->messages = new_msgs;
		ctx->capacity = new_cap;
	}

	/* Sliding window check */
	if (ctx->max_history > 0 && ctx->count >= ctx->max_history) {
		CSILK_LOG_D(
		    "AI context sliding window reached limit (%zu). Evicting oldest message.",
		    ctx->max_history);
		/* Remove oldest message */
		free((char*)ctx->messages[0].role);
		free((char*)ctx->messages[0].content);
		memmove(&ctx->messages[0],
			&ctx->messages[1],
			sizeof(csilk_ai_message_t) * (ctx->count - 1));
		ctx->count--;
	}

	ctx->messages[ctx->count].role = strdup(role);
	ctx->messages[ctx->count].content = strdup(content);
	CSILK_LOG_T("Added message to AI context. Role: '%s', Content: '%.30s...'", role, content);
	ctx->count++;
}

/** @brief Clear all messages from the conversation context.
 *  Frees each message's role and content strings and resets the
 *  message count to zero. The internal array capacity is preserved
 *  to avoid repeated reallocation.
 *  @param ctx Conversation context (may be nullptr). */
void
csilk_ai_context_clear(csilk_ai_context_t* ctx)
{
	if (!ctx) {
		return;
	}
	CSILK_LOG_D("Clearing %zu messages from AI context", ctx->count);
	for (size_t i = 0; i < ctx->count; i++) {
		free((char*)ctx->messages[i].role);
		free((char*)ctx->messages[i].content);
	}
	ctx->count = 0;
}

/** @brief Free a conversation context and all associated resources.
 *  Clears messages, frees the message array, and frees the context struct.
 *  @param ctx Context to free (may be nullptr). */
void
csilk_ai_context_free(csilk_ai_context_t* ctx)
{
	if (!ctx) {
		return;
	}
	CSILK_LOG_D("Freeing AI context with %zu messages", ctx->count);
	csilk_ai_context_clear(ctx);
	free(ctx->messages);
	free(ctx);
}
