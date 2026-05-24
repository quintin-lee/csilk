/**
 * @file context_internal.h
 * @brief Internal definition of csilk_ctx_s.
 * @copyright MIT License
 */

#ifndef CSILK_CONTEXT_INTERNAL_H
#define CSILK_CONTEXT_INTERNAL_H

#include <setjmp.h>
#include <uv.h>

#include "csilk.h"

/** @brief Method-specific handler mapping with metadata for OpenAPI generation.
 */
struct csilk_method_handler_s {
  char* method;              /**< HTTP method string. */
  csilk_handler_t* handlers; /**< Array of handlers for this method. */
  struct csilk_method_handler_s* next; /**< Next method handler in list. */

  /** Metadata for OpenAPI spec generation */
  char* path;              /**< URL path pattern (e.g., "/users/:id"). */
  const char* input_type;  /**< Registered type name for request body binding
                              (optional). */
  const char* output_type; /**< Registered type name for response generation
                              (optional). */
  const char* summary;     /**< Short summary of the operation. */
  const char* description; /**< Detailed description of the operation. */
};
typedef struct csilk_method_handler_s csilk_method_handler_t;

/** @brief Item in context's custom key-value storage. */
typedef struct csilk_storage_item_s {
  char* key;                         /**< Item key name. */
  void* value;                       /**< Pointer to user data. */
  struct csilk_storage_item_s* next; /**< Next item in the linked list. */
} csilk_storage_item_t;

/** @brief Main Request Context.
 *  Holds all information about the current HTTP request/response.
 */
struct csilk_ctx_s {
  int handler_index;         /**< Index of current handler in the chain. */
  csilk_handler_t* handlers; /**< NULL terminated array of handlers. */
  int aborted;               /**< Flag if execution was aborted. */
  jmp_buf jump_buffer;       /**< Buffer for recovery (panic handling). */
  int has_jump_buffer;       /**< Flag if jump_buffer is active. */
  csilk_arena_t* arena;      /**< Request-scoped arena allocator. */
  csilk_request_t request;   /**< Request data. */
  csilk_response_t response; /**< Response data. */
  csilk_param_t params[CSILK_MAX_PARAMS]; /**< URL path parameters array. */
  int params_count; /**< Current number of path parameters. */
  int is_websocket; /**< Flag if connection is upgraded to WebSocket. */
  int is_sse;       /**< Flag if connection is Server-Sent Events. */
  void (*on_ws_message)(csilk_ctx_t* c, const uint8_t* payload, size_t len,
                        int opcode);      /**< WebSocket message callback. */
  csilk_storage_driver_t* storage_driver; /**< Context storage driver. */
  csilk_crypto_driver_t* crypto_driver;   /**< Context crypto driver. */
  csilk_storage_item_t* storage_head; /**< Head of key-value storage list. */
  void* _internal_client; /**< Internal client pointer (DO NOT USE). */
  uv_work_t work_req;     /**< Worker request for async operations. */
  int is_async; /**< Flag if the response will be sent asynchronously. */
  int response_started; /**< Flag if response headers have been sent. */

  /** For OpenAPI spec generation - tracks current method handler */
  csilk_method_handler_t*
      current_handler; /**< Current method handler being executed. */
  char request_id[37]; /**< Unique request ID (UUID-like). */
};

#endif /* CSILK_CONTEXT_INTERNAL_H */
