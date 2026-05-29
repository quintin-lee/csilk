/**
 * @file context_internal.h
 * @brief Internal layout of csilk_ctx_s — the central per-request data
 * structure.
 *
 * This header defines the actual memory layout of the request context struct.
 * It is included ONLY by internal framework code (src/core/). External handlers
 * receive an opaque csilk_ctx_t* and interact through the public API in
 * csilk.h.
 *
 * The context is the single most important data structure in csilk. It is
 * created per-connection, reused across keep-alive requests via
 * csilk_ctx_cleanup(), and carries:
 *   - Parsed HTTP request data (method, path, headers, body, query)
 *   - HTTP response data (status, headers, body)
 *   - URL path parameters captured during routing
 *   - Handler chain state (index, abort flag)
 *   - Error recovery (setjmp/longjmp buffer for middleware recovery)
 *   - Arena allocator for request-scoped memory
 *   - WebSocket/SSE mode flags and callbacks
 *   - Pluggable driver pointers (storage, crypto, cipher)
 *   - Zero-copy file serving state (sendfile fd/offset/size)
 *   - Per-request UUID for tracing
 *
 * @copyright MIT License
 */

#ifndef CSILK_CONTEXT_INTERNAL_H
#define CSILK_CONTEXT_INTERNAL_H

#include <setjmp.h>
#include <uv.h>

#include "csilk/csilk.h"

/**
 * @brief Method-specific handler mapping with OpenAPI metadata and permission
 *        info.
 *
 * Each entry in this linked list represents one HTTP method + handler chain
 * registered at a specific route path. In addition to the handler function
 * array, it carries optional metadata used by:
 *   - OpenAPI spec generation (input_type, output_type, summary, description)
 *   - Permission/ACL checks (perm_required, perm_resource)
 */
struct csilk_method_handler_s {
	char* method;			     /**< HTTP method string (e.g., "GET", "POST"). */
	csilk_handler_t* handlers;	     /**< NULL-terminated array of handler function
                                pointers for this method. */
	struct csilk_method_handler_s* next; /**< Next method handler in this node's
                                         linked list. */

	/** Metadata for OpenAPI spec generation */
	char* path;		 /**< URL path pattern (e.g., "/users/:id"). */
	const char* input_type;	 /**< Registered type name for request body binding
                              (optional, used by csilk_bind_reflect()). */
	const char* output_type; /**< Registered type name for response generation
                              (optional, used by csilk_json_reflect()). */
	const char* summary;	 /**< Short summary of the operation. */
	const char* description; /**< Detailed description of the operation. */

	/** Permission metadata for interface-level access control */
	const char* perm_required; /**< Permission required for this route (e.g.,
                                "read", "write"), or NULL if no check. */
	const char* perm_resource; /**< Resource pattern for permission check (e.g.,
                                "users:*"), or NULL. */
};
typedef struct csilk_method_handler_s csilk_method_handler_t;

/**
 * @brief A single key-value item in the context's custom storage linked list.
 *
 * Items are allocated from the request arena and form a singly-linked list
 * accessible via csilk_set()/csilk_get(). When a storage driver is set on the
 * context, it takes precedence over this simple linked list.
 */
typedef struct csilk_storage_item_s {
	char* key;			   /**< Item key name (arena-allocated). */
	void* value;			   /**< Opaque pointer to user data (not
                                        copied, not freed). */
	struct csilk_storage_item_s* next; /**< Next item in the linked list (NULL if
                                       tail). */
} csilk_storage_item_t;

/**
 * @brief Main Request Context — holds all state for the current HTTP
 *        request/response cycle.
 *
 * ## Lifecycle
 *
 * 1. **Allocation**: Created once per TCP connection in `on_connection()`.
 * 2. **Reset**: `csilk_ctx_cleanup()` is called after each request completes,
 *    which resets arena memory, clears headers/params/body, and resets flags.
 *    The underlying TCP connection and SSL session are preserved.
 * 3. **Reuse**: For keep-alive connections, the same context handles multiple
 *    requests sequentially.
 * 4. **Free**: When the connection closes, the arena is freed and the client
 *    struct (containing the context) is returned to the server's free pool.
 *
 * ## Thread Safety
 *
 * A single context is NEVER accessed from multiple threads simultaneously.
 * The libuv event loop ensures serialized access per connection. Async
 * operations (uv_queue_work) run on the thread pool but access to the context
 * is synchronized via the libuv main-loop callback pattern.
 */
struct csilk_ctx_s {
	/* === Handler Chain State === */
	int handler_index;	   /**< Index of the current handler in the chain; starts at
                        -1 (before first handler). */
	csilk_handler_t* handlers; /**< NULL-terminated array of handler function
                                pointers for the matched route. */
	int aborted;		   /**< Non-zero if handler execution was aborted via csilk_abort().
                  Subsequent csilk_next() calls are no-ops. */

	/* === Error Recovery (setjmp/longjmp) === */
	jmp_buf jump_buffer; /**< setjmp buffer for error recovery (used by
                           panic/recovery middleware via longjmp). */
	int has_jump_buffer; /**< Non-zero if jump_buffer has been initialized and is
                          safe to longjmp to. Guards against longjmp on
                          uninitialized context. */

	/* === Memory Management === */
	csilk_arena_t* arena; /**< Request-scoped arena allocator. Memory is reset
                           between requests. All short-lived allocations
                           (headers, param values, storage items) are served
                           from this arena. */

	/* === Request Data === */
	csilk_request_t request; /**< Parsed HTTP request data (method, path, headers,
                              body, query params, form params). Populated by
                              the llhttp-based HTTP parser. */

	/* === Response Data === */
	csilk_response_t response; /**< HTTP response data (status, headers, body) to
                                be sent to the client. Set by handler functions
                                like csilk_string(), csilk_json(), etc. */

	/* === URL Path Parameters === */
	csilk_param_t params[CSILK_MAX_PARAMS]; /**< URL path parameters captured during routing
                                   (key/value pairs). Populated by the router
                                   when matching parameterized routes like
                                   "/users/:id". */
	int params_count; /**< Number of path parameters currently in params[] array.
                     */

	/* === Protocol Mode Flags === */
	int is_websocket; /**< Non-zero if the connection has been upgraded to
                       WebSocket (set by csilk_ws_handshake). When set, data
                       frames are dispatched to on_ws_message instead of being
                       parsed as HTTP. */
	int is_sse;	  /**< Non-zero if the connection is being used for Server-Sent
                 Events streaming. When set, the framework does not auto-send
                 the response after the handler returns. */

	/** Callback invoked for each incoming WebSocket data frame. Set via
   *  csilk_set_on_ws_message(). Receives the context, unmasked payload,
   *  payload length, and opcode (0x1=text, 0x2=binary). */
	void (*on_ws_message)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

	/* === Pluggable Driver Pointers === */
	csilk_storage_driver_t* storage_driver; /**< Optional pluggable storage backend for
                         csilk_set()/csilk_get(). When set, takes precedence
                         over the internal linked-list storage. Set per-server
                         and propagated to all contexts. */
	csilk_crypto_driver_t* crypto_driver;	/**< Optional pluggable crypto backend
                                           for HMAC, UUID generation, SHA256.
                                           Defaults to OpenSSL-based software
                                           implementation. */
	csilk_cipher_driver_t* cipher_driver;	/**< Optional pluggable cipher backend
                                          for AES-256-GCM encrypt/decrypt,
                                          RSA-OAEP encrypt/decrypt, RSA-PSS
                                          sign/verify, and RSA-2048 key
                                          generation. */

	/* === Simple Key-Value Storage (arena-backed linked list) === */
	csilk_storage_item_t* storage_head; /**< Head of the linked list for simple
                                         arena-backed key-value storage.
                                         Managed by csilk_set()/csilk_get()
                                         when no storage_driver is set. */

	struct csilk_server_s* server; /**< Pointer to the owning server instance. */

	/* === Internal I/O State === */
	void* _internal_client; /**< Opaque pointer to the internal csilk_client_t.
                             MUST NOT be used directly by handlers. Used
                             internally by _csilk_send_data() to route data
                             through TLS or raw TCP. */
	uv_work_t work_req;	/**< libuv work request structure for offloading async
                             operations to the thread pool. Used by
                             csilk_ai_chat_async() and other async handlers. */
	int is_async;		/**< Non-zero if the response will be sent asynchronously
                   (framework skips auto-send after handler chain returns).
                   Set by csilk_response_write() for streaming responses or
                   explicitly by csilk_set_async(). */
	int response_started;	/**< Non-zero if chunked response headers have already
                           been sent to the client. Used by
                           csilk_response_write() to avoid sending headers
                           multiple times in streaming mode. */

	/* === Zero-Copy File Serving (sendfile) === */
	int file_fd;	    /**< File descriptor of the file being sent via sendfile(). -1 if
                  not in use. Set by static file middleware for large file
                  responses. */
	size_t file_offset; /**< Byte offset into the file where sendfile should start
                         reading (for partial/range requests). */
	size_t file_size;   /**< Total number of bytes to send from the file. */

	/** OpenAPI spec generation — tracks the current method handler's metadata */
	csilk_method_handler_t*
	    current_handler; /**< Pointer to the method handler entry for the matched
                          route (NULL if unmatched). Used by
                          csilk_bind_reflect() and csilk_json_reflect() to
                          infer input/output type names from route metadata. */

	/** Per-request unique identifier (UUID v4 string, 36 chars + null). */
	char request_id[37];
};

#endif /* CSILK_CONTEXT_INTERNAL_H */
