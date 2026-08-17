/**
 * @file ctx_types.h
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

#include "csilk/core/internal.h"

/**
 * @brief A single HTTP header stored as a node in a chained hash table.
 */
struct csilk_header_s {
    char*                  key;
    char*                  value;
    size_t                 key_len;
    size_t                 value_len;
    struct csilk_header_s* next;
};
typedef struct csilk_header_s csilk_header_t;

#ifndef CSILK_HEADER_BUCKETS
#define CSILK_HEADER_BUCKETS 64
#endif

/**
 * @brief A fixed-size chained hash table for HTTP headers.
 *
 * Nodes, keys and values are ALL arena-allocated by the map writers
 * (map_set_view/map_set/map_add in header_map.c), so csilk_arena_reset()
 * reclaims everything — cleanup only needs to clear the bucket head
 * pointers of maps that were actually written this request.
 */
struct csilk_header_map_s {
    csilk_header_t* buckets[CSILK_HEADER_BUCKETS];
    uint8_t         used; /**< Set to 1 by map writers; lets cleanup skip zeroing
                     unused maps (avoids a 512-byte memset per map per
                     request). Cleared by the same memset. */
};
typedef struct csilk_header_map_s csilk_header_map_t;

/**
 * @brief Parsed HTTP request — holds all data extracted from the incoming
 *        HTTP request line and headers.
 *
 * Populated by the HTTP/1.1 (llhttp) or HTTP/2 (nghttp2) parser during
 * request finalization.  String fields are arena-allocated and valid until
 * csilk_ctx_cleanup().  request.path is the ONLY exception: it is a
 * The three header maps are large (64-bucket chains ≈ 512 B each) and are
 * placed at the tail so the scalar request core stays compact.
 */

/** @brief Size-class limits and tier definitions for HTTP body cache. */
#define CSILK_BODY_POOL_64KB (64 * 1024)
#define CSILK_BODY_POOL_128KB (128 * 1024)
#define CSILK_BODY_POOL_256KB (256 * 1024)
#define CSILK_BODY_POOL_512KB (524288)
#define CSILK_BODY_POOL_1MB (1048576)

#define CSILK_BODY_POOL_TIER_COUNT 5
#define CSILK_BODY_POOL_MAX_SIZE CSILK_BODY_POOL_1MB
#define CSILK_BODY_POOL_MAX_PER_TIER 8

typedef struct {
    void*  buffers[CSILK_BODY_POOL_MAX_PER_TIER];
    size_t count;
} csilk_body_tier_t;

typedef struct {
    csilk_body_tier_t tiers[CSILK_BODY_POOL_TIER_COUNT];
} csilk_body_pool_t;

/**
 * @brief Mutable HTTP request received from the client.
 *
 * Header maps dominate the struct size (3 maps x 512B buckets = 1536B) and are
 * therefore placed at the tail of the struct so the scalar request core
 * above them stays compact and cache-friendly on the parse/handler hot path.
 */
struct csilk_request_s {
    char*             method;          /**< HTTP method string (e.g., "GET", "POST", "PUT").
                               Arena-allocated copy of the request method. */
    char*             path;            /**< Decoded URL path (e.g., "/users/42").
                               Percent-encoding removed, query string stripped.
                               Malloc'd by csilk_split_url() — freed in cleanup. */
    char*             body;            /**< Raw request body data.
                               For H1: pointer into the recv buffer (not copied).
                               For H2: heap-allocated copy. */
    size_t            body_len;        /**< Byte length of @p body. 0 for GET/HEAD/DELETE
                               or when no Content-Length or Transfer-Encoding
                               is present. */
    size_t            body_capacity;   /**< Allocated buffer capacity (for size-class pool). */
    csilk_ownership_t body_ownership;  /**< Ownership model for request body. */
    int               body_is_managed; /**< Non-zero if body is heap-allocated (H2 realloc),
                              must be freed on cleanup. Zero for H1 bodies
                              (they reference the TCP recv buffer directly). */

    /* --- Header maps (large; kept at the tail — see struct comment) --- */
    csilk_header_map_t headers;      /**< Request headers (key → value) stored in a
                                     fixed-size chained hash table.
                                     Case-insensitive lookup via djb2
                                     hash + strcasecmp. */
    csilk_header_map_t query_params; /**< URL query-string parameters parsed
                                          from the "?" portion of the request
                                          URL. Populated by csilk_parse_query(). */
    csilk_header_map_t form_params;  /**< Form-urlencoded body parameters parsed
                                        from application/x-www-form-urlencoded
                                        body. Populated by
                                        csilk_parse_form_urlencoded(). */
};
typedef struct csilk_request_s csilk_request_t;

/**
 * @brief Mutable HTTP response.
 *
 * Like csilk_request_s, the large header map is kept at the tail so the
 * scalar response core (status/body/body_len) is compact on the handler
 * and response-serialization hot path.
 */
struct csilk_response_s {
    int               status;
    const char*       body;
    size_t            body_len;
    size_t            body_capacity; /**< Allocated buffer capacity (for size-class pool). */
    csilk_ownership_t body_ownership;
    int               body_is_managed;

    csilk_header_map_t headers; /**< Response headers (large; kept at the tail). */
};
typedef struct csilk_response_s csilk_response_t;

/**
 * @brief A single URL path parameter.
 */
struct csilk_param_s {
    char* key;
    char* value;
};
typedef struct csilk_param_s csilk_param_t;

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
    char*            method;        /**< HTTP method string (e.g., "GET", "POST"). */
    csilk_handler_t* handlers;      /**< NULL-terminated array of compiled handler function
                                    pointers for this method (including prepended global middlewares). */
    size_t           handler_count; /**< Number of handlers in the compiled chain. */
    csilk_handler_t* raw_handlers; /**< NULL-terminated array of uncompiled route/group handlers. */
    size_t           raw_handler_count;  /**< Number of uncompiled route/group handlers. */
    struct csilk_method_handler_s* next; /**< Next method handler in this node's
                                             linked list. */

    /** Metadata for OpenAPI spec generation */
    char*       path;        /**< URL path pattern (e.g., "/users/:id"). */
    const char* input_type;  /**< Registered type name for request body binding
                              (optional, used by csilk_bind_reflect()). */
    const char* output_type; /**< Registered type name for response generation
                              (optional, used by csilk_json_reflect()). */
    const char* summary;     /**< Short summary of the operation. */
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
    char* key;                         /**< Item key name (arena-allocated). */
    void* value;                       /**< Opaque pointer to user data. */
    void (*free_fn)(void*);            /**< Optional destructor callback. */
    struct csilk_storage_item_s* next; /**< Next item in the linked list (NULL if
                                         tail). */
} csilk_storage_item_t;

/**
 * @brief Deferred cleanup callback — registered via csilk_ctx_defer().
 *
 * Forms a singly-linked list of cleanup functions that are executed
 * by csilk_ctx_defer_free() when the context is cleaned up or when
 * a longjmp panic occurs.  Nodes are arena-allocated so they are
 * automatically reclaimed by csilk_arena_reset() without individual
 * free calls.
 *
 * Use this to protect heap allocations, file descriptors, and mutex
 * locks held by handlers that may call csilk_panic() — the deferred
 * callbacks run even if the normal handler chain is aborted.
 */
typedef struct csilk_defer_item_s {
    void (*fn)(void* arg);           /**< Cleanup function to invoke. */
    void*                      arg;  /**< Argument forwarded to @p fn. */
    struct csilk_defer_item_s* next; /**< Next item (LIFO order). */
} csilk_defer_item_t;

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
 * The I/O event loop ensures serialized access per connection. Async
 * operations (uv_queue_work) run on the thread pool but access to the context
 * is synchronized via the event-loop callback pattern.
 *
 * ## Memory Layout (hot / cold split)
 *
 * The struct is ordered by access frequency so the per-request hot path
 * (parser, router, handler chain, response send) touches a small cache
 * footprint:
 *
 * - **HOT** — first: dispatch/handler-chain state, arena, request/response
 *   scalar cores, protocol flags. All fit within the first few cache lines.
 * - **Large slabs in the middle**: the four 512-byte header maps and the
 *   embedded URL-params array — needed by every request, but accessed in
 *   bursts (parse / serialize) rather than per-handler-call.
 * - **COLD last**: connection-scoped configuration shared by every request —
 *   pluggable drivers, watermark config, work_req, stream linkage, request_id.
 *
 * This is a single flat struct: no pointer indirection was introduced to
 * split it. Every field keeps its name, so all `c->field` access sites are
 * unchanged.
 */
struct csilk_ctx_s {
    /* ═══════════════════════════════════════════════════════════════════
     * HOT REGION — read/written on every request
     * ═══════════════════════════════════════════════════════════════════ */

    /* === Handler Chain + Dispatch State === */
    int              handler_index; /**< Index of the current handler in the chain; starts at
                        -1 (before first handler). */
    int              aborted;      /**< Non-zero if handler execution was aborted via csilk_abort().
                  Subsequent csilk_next() calls are no-ops. */
    int              panicked;     /**< Non-zero if a handler called csilk_panic().
                                 Subsequent csilk_next() calls are no-ops. */
    int              params_count; /**< Number of path parameters currently in params[] array. */
    csilk_handler_t* handlers;     /**< NULL-terminated array of handler function
                                pointers for the matched route. */
    size_t handler_count;          /**< Total number of handlers in the chain (for bounds check). */
    csilk_defer_item_t* defer_head; /**< Linked list of deferred cleanup callbacks.
                                    Executed by csilk_ctx_defer_free() on
                                    panic and during normal cleanup (LIFO). */

    /* === Memory Management === */
    csilk_arena_t* arena; /**< Request-scoped arena allocator. Memory is reset
                           between requests. All short-lived allocations
                           (headers, param values, storage items) are served
                           from this arena. */

    /* === Internal I/O State (touched on every queued write) === */
    struct csilk_server_s* server;           /**< Pointer to the owning server instance. */
    void*                  _internal_client; /**< Opaque pointer to the internal csilk_client_t.
                             MUST NOT be used directly by handlers. Used
                             internally by _csilk_send_data() to route data
                             through TLS or raw TCP. */
    int conn_closed; /**< Non-zero if the connection has been closed/timed out. */

    /* === Request Data (scalar core; header maps at tail of struct) === */
    csilk_request_t request; /**< Parsed HTTP request data (method, path, headers,
                               body, query params, form params). Populated by
                               the llhttp-based HTTP parser. */

    /* === Response Data (scalar core; header map at tail of struct) === */
    csilk_response_t response; /**< HTTP response data (status, headers, body) to
                                be sent to the client. Set by handler functions
                                like csilk_string(), csilk_json(), etc. */

    /* === Request Flow Flags === */
    int is_async;         /**< Non-zero if the response will be sent asynchronously
                   (framework skips auto-send after handler chain returns).
                   Set by csilk_response_write() for streaming responses or
                   explicitly by csilk_ctx_set_async(). */
    int response_started; /**< Non-zero if chunked response headers have already
                           been sent to the client. Used by
                           csilk_response_write() to avoid sending headers
                           multiple times in streaming mode. */

    /* === Protocol Mode Flags === */
    int is_websocket; /**< Non-zero if the connection has been upgraded to
                       WebSocket (set by csilk_ws_handshake). When set, data
                       frames are dispatched to on_ws_message instead of being
                       parsed as HTTP. */
    int is_sse;       /**< Non-zero if the connection is being used for Server-Sent
                 Events streaming. When set, the framework does not auto-send
                 the response after the handler returns. */

    /** Callback invoked for each incoming WebSocket data frame. Set via
   *  csilk_set_on_ws_message(). Receives the context, unmasked payload,
   *  payload length, and opcode (0x1=text, 0x2=binary). */
    void (*on_ws_message)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

    /** Callback invoked for each outgoing WebSocket data frame (for testing). */
    void (*on_ws_send)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

    /* === Streaming Backpressure & Flow Control === */
    size_t write_high_water_mark; /**< Outbound write queue high water mark (bytes). */
    size_t write_low_water_mark;  /**< Outbound write queue low water mark (bytes). */
    size_t max_write_buffer_size; /**< Hard max write buffer size before drop (bytes). */
    int    write_paused;          /**< 1 if backpressure paused the stream. */
    void (*on_drain)(struct csilk_ctx_s* c, void* user_data); /**< Drain callback. */
    void* on_drain_data;

    /* ═══════════════════════════════════════════════════════════════════
     * LARGE SLABS — touched in bursts by parse / route / serialize
     * ═══════════════════════════════════════════════════════════════════ */

    /* === URL Path Parameters (embedded array) === */
    csilk_param_t params[CSILK_MAX_PARAMS]; /**< URL path parameters captured during routing
                                   (key/value pairs). Populated by the router
                                   when matching parameterized routes like
                                   "/users/:id". */

    /* === Zero-Copy File Serving (sendfile) === */
    int    file_fd;     /**< File descriptor of the file being sent via sendfile(). -1 if
                  not in use. Set by static file middleware for large file
                  responses. */
    size_t file_offset; /**< Byte offset into the file where sendfile should start
                         reading (for partial/range requests). */
    size_t file_size;   /**< Total number of bytes to send from the file. */

    /* === Simple Key-Value Storage (arena-backed linked list) === */
    csilk_storage_item_t* storage_head; /**< Head of the linked list for simple
                                         arena-backed key-value storage.
                                         Managed by csilk_set()/csilk_get()
                                         and freed on context cleanup. */

    /** OpenAPI spec generation — tracks the current method handler's metadata */
    csilk_method_handler_t*
        current_handler; /**< Pointer to the method handler entry for the matched
                          route (NULL if unmatched). Used by
                          csilk_bind_reflect() and csilk_json_reflect() to
                          infer input/output type names from route metadata. */

    /* === Zero-Copy Receive Buffers (connection-scoped containers) === */
    char**  read_buffers;
    int     read_buffers_count;
    int     read_buffers_capacity;
    char*   read_buffers_embedded[16];
    size_t* read_buf_sizes;              /**< Parallel size tracker — embedded or heap. */
    size_t  read_buf_sizes_embedded[16]; /**< Embedded size storage. */

    /* ═══════════════════════════════════════════════════════════════════
     * COLD REGION — connection-scoped configuration, shared by requests
     * ═══════════════════════════════════════════════════════════════════ */

    /* === Pluggable Driver Pointers (set from the server at init) === */
    csilk_storage_driver_t* storage_driver; /**< Optional pluggable storage backend for
                         csilk_set()/csilk_get(). When set, takes precedence
                         over the internal linked-list storage. Set per-server
                         and propagated to all contexts. */
    csilk_crypto_driver_t*  crypto_driver;  /**< Optional pluggable crypto backend
                                           for HMAC, UUID generation, SHA256.
                                           Defaults to OpenSSL-based software
                                           implementation. */
    csilk_cipher_driver_t*  cipher_driver;  /**< Optional pluggable cipher backend
                                          for AES-256-GCM encrypt/decrypt,
                                          RSA-OAEP encrypt/decrypt, RSA-PSS
                                          sign/verify, and RSA-2048 key
                                          generation. */

    /* === HTTP/2 Stream Support === */
    int32_t stream_id; /**< HTTP/2 Stream ID. 0 for HTTP/1.1 connections. */
    struct csilk_ctx_s*
        next_stream;   /**< Linked list of active multiplexed contexts for a single client. */

    csilk_io_work_t work_req; /**< I/O work request structure for offloading async
                     operations to the thread pool. Used by
                     csilk_ai_chat_async() and other async handlers. */

    /** Per-request unique identifier (UUID v4 string, 36 chars + null). */
    char request_id[CSILK_UUID_BUF_SIZE];
};

/** @brief Internal context initialiser. */
CSILK_INTERNAL void _csilk_ctx_init(csilk_ctx_t* c, struct csilk_server_s* s, void* client);

/** @brief Register a raw read buffer with the context for zero-copy view lifetime management. */
CSILK_INTERNAL int _csilk_ctx_register_read_buffer(csilk_ctx_t* c, char* base);

/** @brief Register a pool-backed read buffer, tracking its size for pool return on cleanup. */
CSILK_INTERNAL int _csilk_ctx_register_pooled_read_buffer(csilk_ctx_t* c, char* base, size_t size);

/* === Async/Multi-Worker Loop Support === */
CSILK_INTERNAL csilk_io_loop_t* _csilk_ctx_loop(csilk_ctx_t* c);
CSILK_INTERNAL void             _csilk_ctx_async_ref_incr(csilk_ctx_t* c);
CSILK_INTERNAL void             _csilk_ctx_async_ref_decr(csilk_ctx_t* c);

#endif /* CSILK_CONTEXT_INTERNAL_H */
