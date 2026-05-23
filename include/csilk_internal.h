/**
 * @file csilk_internal.h
 * @brief Internal header for SHA1, Base64, and WebSocket frame parsing.
 * @copyright MIT License
 */

#ifndef CSILK_INTERNAL_H
#define CSILK_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <uv.h>

#include "csilk.h"

/** @brief Item in context's custom key-value storage. */
typedef struct csilk_storage_item_s {
  char* key;               /**< Item key name. */
  void* value;             /**< Pointer to user data. */
  struct csilk_storage_item_s* next; /**< Next item in the linked list. */
} csilk_storage_item_t;

/** @brief Main Request Context.
 * Holds all information about the current HTTP request/response.
 */
struct csilk_ctx_s {
  int handler_index;        /**< Index of current handler in the chain. */
  csilk_handler_t* handlers;  /**< NULL terminated array of handlers. */
  int aborted;              /**< Flag if execution was aborted. */
  jmp_buf jump_buffer;      /**< Buffer for recovery (panic handling). */
  int has_jump_buffer;      /**< Flag if jump_buffer is active. */
  csilk_arena_t* arena;       /**< Request-scoped arena allocator. */
  csilk_request_t request;    /**< Request data. */
  csilk_response_t response;  /**< Response data. */
  csilk_param_t params[CSILK_MAX_PARAMS]; /**< URL path parameters array. */
  int params_count;         /**< Current number of path parameters. */
  int is_websocket;         /**< Flag if connection is upgraded to WebSocket. */
  int is_sse;               /**< Flag if connection is Server-Sent Events. */
  void (*on_ws_message)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode); /**< WebSocket message callback. */
  csilk_storage_item_t* storage_head; /**< Head of key-value storage list. */
  void* _internal_client;   /**< Internal client pointer (DO NOT USE). */
  uv_work_t work_req;       /**< Worker request for async operations. */
  int is_async;             /**< Flag if the response will be sent asynchronously. */
  int response_started;     /**< Flag if response headers have been sent. */
};

/** @brief SHA1 hashing context. */
typedef struct {
    uint32_t state[5];   /**< Intermediate hash state. */
    uint32_t count[2];   /**< Message length counter. */
    uint8_t buffer[64];   /**< Data block buffer. */
} csilk_sha1_ctx;

/** @brief Initialize a SHA1 context.
 * @param context SHA1 context to initialize. */
void csilk_sha1_init(csilk_sha1_ctx* context);

/** @brief Feed data into the SHA1 hashing context.
 * @param context SHA1 context.
 * @param data Input data bytes.
 * @param len Length of input data. */
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len);

/** @brief Finalize SHA1 hash and produce the digest.
 * @param context SHA1 context.
 * @param digest Output buffer for 20-byte hash. */
void csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20]);

/** @brief Encode raw bytes as Base64 string.
 * @param src Source byte buffer.
 * @param len Length of source data.
 * @param out Output buffer (must be at least ((len+2)/3)*4+1 bytes). */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out);

/** @brief Parse incoming WebSocket frame data.
 * @param c Request context.
 * @param buf Raw input buffer.
 * @param nread Number of bytes read. */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread);

/** @brief Internal: Trigger response sending (used for async offloading).
 * @param c Request context. */
void _csilk_send_response(csilk_ctx_t* c);

/** @brief URL decode a string in-place.
 * @param str The string to decode.
 * @return The length of the decoded string. */
size_t csilk_url_decode(char* str);

#endif
