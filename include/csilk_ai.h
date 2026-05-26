/**
 * @file csilk_ai.h
 * @brief Unified interface for AI/LLM service integration.
 *
 * Provides a provider-agnostic abstraction for chat completions, embeddings,
 * and other AI services. Supports pluggable drivers (e.g., OpenAI, Claude, 
 * local LLMs).
 *
 * @copyright MIT License
 */

#ifndef CSILK_AI_H
#define CSILK_AI_H

#include <stddef.h>
#include <stdbool.h>

/** @brief Opaque handle for an AI provider instance. */
typedef struct csilk_ai_s csilk_ai_t;

/** @brief A single message in a chat conversation. */
typedef struct {
  const char* role;    /**< Message role (e.g., "system", "user", "assistant"). */
  const char* content; /**< Message text content. */
} csilk_ai_message_t;

/** @brief Callback for streaming mode. 
 * @param chunk The text content delta.
 * @param user_data User-provided context. */
typedef void (*csilk_ai_stream_cb)(const char* chunk, void* user_data);

/** @brief Request parameters for chat completion. */
typedef struct {
  const char* model;             /**< Provider-specific model name (e.g., "gpt-4"). */
  csilk_ai_message_t* messages;  /**< Array of conversation messages. */
  size_t message_count;          /**< Number of messages in the array. */
  double temperature;            /**< Sampling temperature (0.0 to 2.0). */
  double top_p;                  /**< Nucleus sampling probability. */
  double presence_penalty;       /**< Penalty for new topics (-2.0 to 2.0). */
  double frequency_penalty;      /**< Penalty for repetitive tokens (-2.0 to 2.0). */
  int max_tokens;                /**< Maximum tokens to generate. */
  const char** stop;             /**< Array of stop sequences. */
  size_t stop_count;             /**< Number of stop sequences. */
  const char* user;              /**< Unique identifier for the end-user. */
  bool stream;                   /**< Enable streaming mode. */
  csilk_ai_stream_cb on_chunk;   /**< Callback invoked for each chunk in stream mode. */
  void* user_data;               /**< User context for the stream callback. */
} csilk_ai_chat_request_t;

/** @brief Response data from a chat completion. */
typedef struct {
  char* content;            /**< Generated text content (heap-allocated). */
  int prompt_tokens;        /**< Tokens used in the prompt. */
  int completion_tokens;    /**< Tokens used in the generation. */
  int total_tokens;          /**< Total tokens used. */
  char* raw_response;       /**< Full raw JSON response (optional, heap-allocated). */
  char* error_message;      /**< Detailed error message if call failed (heap-allocated). */
} csilk_ai_chat_response_t;

/** @brief Driver interface for AI providers. */
typedef struct {
  const char* name;
  /** @brief Initialize the driver-specific state. */
  void* (*init)(const char* api_key, const char* base_url);
  /** @brief Perform a synchronous chat completion call. */
  int (*chat)(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res);
  /** @brief Clean up driver-specific state. */
  void (*free)(void* state);
} csilk_ai_driver_t;

/* --- Core API --- */

/** @brief Create a new AI instance with a specific driver.
 * @param driver_name "openai" or other registered driver names.
 * @param api_key     API key for the provider.
 * @param base_url    Optional custom base URL (pass NULL for provider default).
 * @return New AI handle, or NULL if driver not found or init failed. */
csilk_ai_t* csilk_ai_new(const char* driver_name, const char* api_key, const char* base_url);

/** @brief Perform a chat completion.
 * @param ai  AI handle.
 * @param req Chat request parameters.
 * @param res [out] Chat response to populate.
 * @return 0 on success, -1 on failure. */
int csilk_ai_chat(csilk_ai_t* ai, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res);

/** @brief Free an AI handle. */
void csilk_ai_free(csilk_ai_t* ai);

/** @brief Free a chat response structure. */
void csilk_ai_chat_response_free(csilk_ai_chat_response_t* res);

/** @brief Register a new AI driver.
 * @note Not thread-safe during registration. Call at startup. */
void csilk_ai_register_driver(const csilk_ai_driver_t* driver);

#endif /* CSILK_AI_H */
