#pragma once
/**
 * @file ai.h
 * @brief Unified pluggable interface for AI/LLM service integration.
 *
 * Provides a provider-agnostic abstraction for chat completions, embeddings,
 * streaming, and tool/function calling.  Supports multiple backend drivers
 * (OpenAI, Claude, Ollama, etc.) registered via csilk_ai_register_driver.
 *
 * ## Architecture
 * The AI layer has three tiers:
 *   1. **Driver** — a csilk_ai_driver_t vtable implementing the actual
 *      HTTP/REST calls to a specific provider.
 *   2. **Instance** — created by csilk_ai_new(), wraps a driver + API config.
 *   3. **Context** — csilk_ai_context_t manages conversation history with
 *      a sliding-window FIFO (for maintaining chat context across turns).
 *
 * ## Thread Safety
 * The driver functions are synchronous by default.  Asynchronous variants
 * (csilk_ai_chat_async) offload work to the I/O thread pool and invoke
 * the callback on the main event loop, making them safe for use from
 * request handlers.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "csilk/core/json.h"

/** @brief Maximum number of AI driver backends that can be registered. */
enum { MAX_DRIVERS = 8 };

/**
 * @brief AI Engine statistics.
 */
typedef struct {
    uint64_t requests_total;    /**< Total chat/embeddings calls. */
    uint64_t tokens_total;      /**< Total tokens (prompt + completion). */
    uint64_t prompt_tokens;     /**< Total prompt tokens. */
    uint64_t completion_tokens; /**< Total completion tokens. */
    uint64_t errors_total;      /**< Total failed AI calls. */
    uint64_t duration_us_total; /**< Cumulative duration in microseconds. */
} csilk_ai_stats_t;

/**
 * @brief Get current AI engine statistics.
 * @param stats [out] Pointer to stats struct to populate.
 */
void csilk_ai_get_stats(csilk_ai_stats_t* stats);

/**
 * @brief Convert AI statistics to a JSON string.
 * @param stats Pointer to stats struct.
 * @return Heap-allocated JSON string (must be freed).
 */
char* csilk_ai_stats_to_json(const csilk_ai_stats_t* stats);

/**
 * @brief Register a WebSocket monitor for real-time AI events.
 * @param c Framework context (WebSocket connection).
 */
void csilk_ai_register_monitor(void* c);

/**
 * @brief Unregister a WebSocket monitor from AI events.
 * @param c Framework context to unregister.
 */
void csilk_ai_unregister_monitor(void* c);

/** @brief Opaque handle for an AI provider instance. */
typedef struct csilk_ai_s csilk_ai_t;

/** @brief a tool call requested by the model. */
typedef struct {
    char* id;        /**< Unique call ID. */
    char* name;      /**< Function name to call. */
    char* arguments; /**< JSON-formatted arguments string. */
} csilk_ai_tool_call_t;

/** @brief A single message in a chat conversation. */
typedef struct {
    const char* role;    /**< Message role ("system", "user", "assistant", "tool"). */
    const char* content; /**< Message text content (NULL when role=="assistant" with tool_calls). */
    csilk_ai_tool_call_t*
        tool_calls; /**< Tool calls requested by the model (NULL if none; only valid when role=="assistant"). */
    size_t      tool_call_count; /**< Number of entries in tool_calls (0 when NULL). */
    const char* tool_call_id;    /**< Tool call result ID (NULL unless role=="tool"). */
} csilk_ai_message_t;

/** @brief Callback for streaming mode.
 * @param chunk The text content delta.
 * @param user_data User-provided context. */
typedef void (*csilk_ai_stream_cb)(const char* chunk, void* user_data);

/** @brief A function tool definition for the AI model. */
typedef struct {
    const char* name;            /**< Function name. */
    const char* description;     /**< Description of what the function does. */
    void*       parameters_json; /**< JSON Schema of parameters (csilk_json_t*). */
} csilk_ai_tool_function_t;

/** @brief A tool that can be called by the model. */
typedef struct {
    const char*              type; /**< Currently only "function" is supported. */
    csilk_ai_tool_function_t function;
} csilk_ai_tool_t;

/** @brief Request parameters for chat completion. */
typedef struct {
    const char*         model;             /**< Provider-specific model name (e.g., "gpt-4"). */
    csilk_ai_message_t* messages;          /**< Array of conversation messages. */
    size_t              message_count;     /**< Number of messages in the array. */
    double              temperature;       /**< Sampling temperature (0.0 to 2.0). */
    double              top_p;             /**< Nucleus sampling probability. */
    double              presence_penalty;  /**< Penalty for new topics (-2.0 to 2.0). */
    double              frequency_penalty; /**< Penalty for repetitive tokens (-2.0 to 2.0). */
    int                 max_tokens;        /**< Maximum tokens to generate. */
    const char**        stop;              /**< Array of stop sequences. */
    size_t              stop_count;        /**< Number of stop sequences. */
    const char*         user;              /**< Unique identifier for the end-user. */
    bool                stream;            /**< Enable streaming mode. */
    csilk_ai_stream_cb  on_chunk;          /**< Callback invoked for each chunk in stream mode. */
    void*               user_data;         /**< User context for the stream callback. */
    int                 timeout_ms;        /**< Request timeout in milliseconds (0 for default). */
    csilk_ai_tool_t*    tools;             /**< Available tools for the model. */
    size_t              tool_count;        /**< Number of tools. */
    const char*         tool_choice;       /**< "none", "auto", or "required". */
    const char*
        reasoning_effort; /**< "low", "medium", or "high" for extended reasoning models (o1/o3). */
} csilk_ai_chat_request_t;

/** @brief Response data from a chat completion. */
typedef struct {
    char*                 content;           /**< Generated text content (heap-allocated). */
    csilk_ai_tool_call_t* tool_calls;        /**< Array of tool calls (heap-allocated). */
    size_t                tool_call_count;   /**< Number of tool calls. */
    int                   prompt_tokens;     /**< Tokens used in the prompt. */
    int                   completion_tokens; /**< Tokens used in the generation. */
    int                   total_tokens;      /**< Total tokens used. */
    char*                 raw_response; /**< Full raw JSON response (optional, heap-allocated). */
    char* error_message; /**< Detailed error message if call failed (heap-allocated). */
} csilk_ai_chat_response_t;

/** @brief Response data for embeddings. */
typedef struct {
    float* values;    /**< Flattened array of vector values. */
    size_t dimension; /**< Dimension of a single vector. */
    size_t count;     /**< Number of vectors in the array. */
    int    prompt_tokens;
    int    total_tokens;
    char*  error_message;
} csilk_ai_embeddings_response_t;

/** @brief Callback for asynchronous chat completion. */
typedef void (*csilk_ai_chat_async_cb)(int status, csilk_ai_chat_response_t* res, void* user_data);

/** @brief Callback for asynchronous embeddings. */
typedef void (*csilk_ai_embeddings_async_cb)(int                             status,
                                             csilk_ai_embeddings_response_t* res,
                                             void*                           user_data);

/**
 * @brief Virtual function table implemented by each AI provider backend.
 *
 * Each registered driver (OpenAI, Claude, Ollama, etc.) provides these four
 * functions.  The @p init function returns a driver-specific state handle
 * that is passed to all subsequent calls.
 */
typedef struct {
    const char* name; /**< Driver identifier (e.g., "openai", "ollama"). Must
                       match the name passed to csilk_ai_new. */
    /** @brief Initialize driver-specific state (e.g., HTTP client, auth tokens).
   *  @param api_key  API key (may be NULL for providers that don't need one).
   *  @param base_url Optional custom endpoint URL (NULL for provider default).
   *  @return Opaque driver state handle, or NULL on failure. */
    void* (*init)(const char* api_key, const char* base_url);
    /** @brief Perform a synchronous chat completion call.
   *  @param state Driver state from init().
   *  @param req   Request parameters (model, messages, temperature, tools...).
   *  @param res   [out] Populated response (content, token counts, tool calls).
   *  @return 0 on success, -1 on failure. */
    int (*chat)(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res);
    /** @brief Perform a synchronous embeddings call.
   *  @param state Driver state from init().
   *  @param model Model name for embeddings.
   *  @param input Array of input strings to embed.
   *  @param count Number of input strings.
   *  @param res   [out] Populated embeddings response.
   *  @return 0 on success, -1 on failure. */
    int (*embeddings)(void*                           state,
                      const char*                     model,
                      const char**                    input,
                      size_t                          count,
                      csilk_ai_embeddings_response_t* res);
    /** @brief Clean up all driver-specific state.
   *  @param state Driver state to free (from init()). */
    void (*free)(void* state);
} csilk_ai_driver_t;

/** @brief Helper to manage conversation context and history. */
typedef struct {
    csilk_ai_message_t* messages;    /**< Array of history messages. */
    size_t              count;       /**< Number of messages. */
    size_t              capacity;    /**< Allocated capacity. */
    size_t              max_history; /**< Maximum messages to keep (0 for unlimited). */
} csilk_ai_context_t;

/* --- Core API --- */

/** @brief Create a new AI instance with a specific driver.
 * @param driver_name "openai", "ollama", or other registered driver names.
 * @param api_key     API key for the provider (may be NULL for Ollama).
 * @param base_url    Optional custom base URL (pass NULL for provider default).
 * @return New AI handle, or NULL if driver not found or init failed. */
csilk_ai_t* csilk_ai_new(const char* driver_name, const char* api_key, const char* base_url);

/** @brief Perform a chat completion.
 * @param ai  AI handle.
 * @param req Chat request parameters.
 * @param res [out] Chat response to populate.
 * @return 0 on success, -1 on failure. */
int
csilk_ai_chat(csilk_ai_t* ai, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res);

/** @brief Perform an asynchronous chat completion.
 * @note Uses the framework's internal thread pool. The callback is invoked on
 * the main loop. */
void csilk_ai_chat_async(csilk_ai_t*                    ai,
                         const csilk_ai_chat_request_t* req,
                         csilk_ai_chat_async_cb         cb,
                         void*                          user_data);

/** @brief Generate embeddings for the given input strings.
 * @param ai     AI handle.
 * @param model  Model name (e.g., "text-embedding-3-small").
 * @param input  Array of strings to embed.
 * @param count  Number of strings.
 * @param res    [out] Embeddings response to populate.
 * @return 0 on success, -1 on failure. */
int csilk_ai_embeddings(csilk_ai_t*                     ai,
                        const char*                     model,
                        const char**                    input,
                        size_t                          count,
                        csilk_ai_embeddings_response_t* res);

/** @brief Generate embeddings asynchronously. */
void csilk_ai_embeddings_async(csilk_ai_t*                  ai,
                               const char*                  model,
                               const char**                 input,
                               size_t                       count,
                               csilk_ai_embeddings_async_cb cb,
                               void*                        user_data);

/** @brief Free an AI handle. */
void csilk_ai_free(csilk_ai_t* ai);

/** @brief Free a chat response structure. */
void csilk_ai_chat_response_free(csilk_ai_chat_response_t* res);

/** @brief Free an embeddings response structure. */
void csilk_ai_embeddings_response_free(csilk_ai_embeddings_response_t* res);

/** @brief Register a new AI driver.
 * @note Not thread-safe during registration. Call at startup. */
void csilk_ai_register_driver(const csilk_ai_driver_t* driver);

/* --- Context Helpers --- */

/** @brief Initialize a new conversation context.
 * @param max_history Maximum number of messages to keep (FIFO sliding window). */
csilk_ai_context_t* csilk_ai_context_new(size_t max_history);

/** @brief Add a message to the context.
 * @note Strings are duplicated internally. */
void csilk_ai_context_add(csilk_ai_context_t* ctx, const char* role, const char* content);

/** @brief Add a tool result message to the context.
 * @note Creates an assistant message with tool_calls from the response,
 *       followed by tool messages with results. Strings are duplicated internally. */
void csilk_ai_context_add_tool_result(csilk_ai_context_t* ctx, const csilk_ai_chat_response_t* res);

/** @brief Clear all messages from the context. */
void csilk_ai_context_clear(csilk_ai_context_t* ctx);

/** @brief Free a conversation context. */
void csilk_ai_context_free(csilk_ai_context_t* ctx);

/* --- Full Lifecycle AI Trace & Cost Monitoring --- */

/**
 * @brief Context for end-to-end AI latency, token throughput, and cost tracing.
 */
typedef struct csilk_ai_trace_s {
    int64_t         id;                /**< Trace identifier */
    int64_t         user_id;           /**< User ID */
    int64_t         session_id;        /**< Session / conversation ID */
    char            provider[64];      /**< AI provider ("openai", "ollama", "deepseek") */
    char            model[128];        /**< AI model identifier */
    char*           input_messages;    /**< Serialized input messages JSON */
    char*           output_content;    /**< Accumulated completion text */
    char*           system_prompt;     /**< System prompt */
    int             prompt_tokens;     /**< Prompt tokens */
    int             completion_tokens; /**< Completion tokens */
    int             total_tokens;      /**< Total tokens */
    long            latency_ms;        /**< Total latency in milliseconds */
    long            first_token_ms;    /**< Time to First Token (TTFT) in ms */
    double          tokens_per_sec;    /**< Generation throughput (tokens/sec) */
    double          cost_usd;          /**< Estimated cost in USD */
    double          temperature;       /**< Sampling temperature */
    int             max_tokens;        /**< Max tokens parameter */
    double          top_p;             /**< Top-p parameter */
    char            status[32];        /**< Completion status ("ok", "error") */
    char*           error_message;     /**< Error message if failed */
    char*           metadata;          /**< Custom metadata JSON */
    struct timespec t_start;           /**< Monotonic start timestamp */
    struct timespec t_first_token;     /**< Monotonic first token timestamp */
    struct timespec t_end;             /**< Monotonic end timestamp */
    int             has_first_token;   /**< Flag whether first token was recorded */
    long            accumulated_len;   /**< Accumulated length of output bytes */
    char            tool_spans[3072];  /**< Formatted tool spans JSON array */
} csilk_ai_trace_t;

/**
 * @brief Initialize a trace struct on the stack or existing buffer.
 */
void csilk_ai_trace_init(csilk_ai_trace_t* t, int64_t user_id, int64_t session_id);

/**
 * @brief Allocate and initialize a new trace struct on the heap.
 */
csilk_ai_trace_t* csilk_ai_trace_new(int64_t user_id, int64_t session_id);

/**
 * @brief Free resources associated with a trace struct.
 */
void csilk_ai_trace_free(csilk_ai_trace_t* t);

/**
 * @brief Set provider and model identifiers on trace.
 */
void csilk_ai_trace_set_provider(csilk_ai_trace_t* t, const char* provider, const char* model);

/**
 * @brief Set inference parameters on trace.
 */
void
csilk_ai_trace_set_params(csilk_ai_trace_t* t, double temperature, int max_tokens, double top_p);

/**
 * @brief Set system prompt on trace.
 */
void csilk_ai_trace_set_system_prompt(csilk_ai_trace_t* t, const char* prompt);

/**
 * @brief Append output text chunk and track first token timing if not yet recorded.
 */
void csilk_ai_trace_append_output(csilk_ai_trace_t* t, const char* chunk);

/**
 * @brief Explicitly record first token arrival time (TTFT).
 */
void csilk_ai_trace_record_first_token(csilk_ai_trace_t* t);

/**
 * @brief Heuristically estimate token count from mixed ASCII and CJK text.
 */
int csilk_ai_estimate_tokens_from_text(const char* text);

/**
 * @brief Calculate prompt and completion tokens and estimated cost in USD.
 */
void csilk_ai_trace_calculate_tokens_and_cost(csilk_ai_trace_t* t,
                                              int               prompt_tokens,
                                              int               completion_tokens);

/**
 * @brief Append a tool span execution record to the trace.
 */
void csilk_ai_trace_add_tool_span(
    csilk_ai_trace_t* t, const char* name, long latency_ms, size_t bytes, int ok);

/**
 * @brief Finish trace measurement, compute total latency and tokens/sec throughput.
 */
void csilk_ai_trace_finish(csilk_ai_trace_t* t, const char* status, const char* error);

/**
 * @brief Serialize trace to a JSON object.
 */
csilk_json_t* csilk_ai_trace_to_json(const csilk_ai_trace_t* t);
