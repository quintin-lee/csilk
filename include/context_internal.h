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

/** @brief A single key-value item in the context's custom storage linked list.
 *
 * Items are allocated from the request arena and form a singly-linked list
 * accessible via csilk_set()/csilk_get(). */
typedef struct csilk_storage_item_s {
  char* key;                         /**< Item key name. */
  void* value;                       /**< Pointer to user data. */
  struct csilk_storage_item_s* next; /**< Next item in the linked list. */
} csilk_storage_item_t;

/** @brief Main Request Context — holds all state for the current HTTP
 * request/response cycle.
 *
 * This structure is the central data object passed through the handler chain.
 * It contains request data (method, path, headers, body, query params),
 * response data (status, headers, body), URL path parameters captured during
 * routing, storage for handler-injected values, arena allocator for
 * request-scoped memory, and metadata for WebSocket/SSE/async responses.
 *
 * The context is reused across keep-alive requests via csilk_ctx_cleanup(),
 * which resets arena memory and clears per-request state while preserving
 * the underlying TCP connection state.
 */
struct csilk_ctx_s {
  int handler_index; /**< Index of the current handler in the chain; starts at
                        -1. */
  csilk_handler_t* handlers; /**< NULL-terminated array of handler function
                                pointers for the matched route. */
  int aborted; /**< Non-zero if handler execution was aborted via csilk_abort().
                */
  jmp_buf jump_buffer;  /**< setjmp buffer for error recovery (used by
                           panic/recovery middleware). */
  int has_jump_buffer;  /**< Non-zero if jump_buffer has been initialized and is
                           safe to longjmp to. */
  csilk_arena_t* arena; /**< Request-scoped arena allocator. Memory is reset
                           between requests. */
  csilk_request_t request; /**< Parsed HTTP request data (method, path, headers,
                              body, query). */
  csilk_response_t response; /**< HTTP response data (status, headers, body) to
                                be sent to the client. */
  csilk_param_t
      params[CSILK_MAX_PARAMS]; /**< URL path parameters captured during routing
                                   (key/value pairs). */
  int params_count; /**< Number of path parameters currently in params[] array.
                     */
  int is_websocket; /**< Non-zero if the connection has been upgraded to
                       WebSocket (set by csilk_ws_handshake). */
  int is_sse; /**< Non-zero if the connection is being used for Server-Sent
                 Events streaming. */
  void (*on_ws_message)(csilk_ctx_t* c, const uint8_t* payload, size_t len,
                        int opcode); /**< Callback invoked for each incoming
                                        WebSocket data frame. */
  csilk_storage_driver_t*
      storage_driver; /**< Optional pluggable storage backend for
                         csilk_set()/csilk_get(). */
  csilk_crypto_driver_t* crypto_driver; /**< Optional pluggable crypto backend
                                           for HMAC, UUID, etc. */
  csilk_storage_item_t* storage_head;   /**< Head of the linked list for simple
                                           arena-backed key-value storage. */
  void* _internal_client; /**< Opaque pointer to the internal csilk_client_t.
                             MUST NOT be used directly by handlers. */
  uv_work_t work_req;     /**< libuv work request structure for offloading async
                             operations to the thread pool. */
  int is_async; /**< Non-zero if the response will be sent asynchronously
                   (framework skips auto-send). */
  int response_started; /**< Non-zero if chunked response headers have already
                           been sent to the client. */

  /* For zero-copy file serving via sendfile() */
  int file_fd; /**< File descriptor of the file being sent via sendfile(). -1 if
                  not in use. */
  size_t file_offset; /**< Byte offset into the file where sendfile should start
                         reading. */
  size_t file_size;   /**< Total number of bytes to send from the file. */

  /** For OpenAPI spec generation — tracks the current method handler's metadata
   */
  csilk_method_handler_t*
      current_handler; /**< Pointer to the method handler entry for the matched
                          route (NULL if unmatched). */
  char request_id[37]; /**< Per-request unique identifier (UUID v4 string, 36
                          chars + null). */
};

#endif /* CSILK_CONTEXT_INTERNAL_H */
