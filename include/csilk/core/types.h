#pragma once
/**
 * @file types.h
 * @brief Core data types for the csilk HTTP web framework.
 *
 * Defines all public structs, typedefs, enums, and key constants
 * needed by the request context, router, server, middleware,
 * WebSocket, SSE, and utility APIs.
 *
 * @version 0.5.2
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "csilk/core/errors.h"
#include "csilk/version.h"

/** @brief Forward declaration of opaque JSON value type. */
typedef struct csilk_json_s csilk_json_t;

/** @brief Internal visibility macro for symbols shared across csilk sub-modules. */
#if defined(__GNUC__) && __GNUC__ >= 4
#define CSILK_INTERNAL __attribute__((visibility("default")))
#else
#define CSILK_INTERNAL
#endif

/** @brief 64-byte Cache-Line Alignment macro to prevent False Sharing across CPU cores. */
#if defined(_MSC_VER)
#define CSILK_CACHE_ALIGNED __declspec(align(64))
#elif defined(__GNUC__) || defined(__clang__)
#define CSILK_CACHE_ALIGNED __attribute__((aligned(64)))
#else
#define CSILK_CACHE_ALIGNED
#endif

/**
 * @brief Maximum number of URL path parameters that can be extracted from a
 * single request.  Parameters beyond this limit are silently ignored.
 * Tune if your routes contain more than 20 dynamic segments.
 * Overridable via CMake -DCSILK_MAX_PARAMS=<N> or by defining before include.
 */
#ifndef CSILK_MAX_PARAMS
#define CSILK_MAX_PARAMS 20
#endif

/**
 * @brief Maximum number of items that can be stored in the context key-value storage.
 *
 * This limit prevents uncontrolled memory consumption in the request arena
 * by preventing a single request from setting an excessive number of keys.
 * Overridable via CMake -DCSILK_MAX_STORAGE=<N> or by defining before include.
 */
#ifndef CSILK_MAX_STORAGE
#define CSILK_MAX_STORAGE 64
#endif

/** @brief Arena chunk size tiers for adaptive caching. */
enum { CSILK_ARENA_TIER_SMALL = 0 };  /* 4KB - standard requests */
enum { CSILK_ARENA_TIER_MEDIUM = 1 }; /* 16KB - large forms/JSON */
enum { CSILK_ARENA_TIER_LARGE = 2 };  /* 64KB - file uploads/large responses */

/** @brief Number of arena chunk size tiers. */
#ifndef CSILK_ARENA_TIER_COUNT
#define CSILK_ARENA_TIER_COUNT 3
#endif

/** @brief Default request arena chunk size in bytes. */
#ifndef CSILK_DEFAULT_ARENA_SIZE
#define CSILK_DEFAULT_ARENA_SIZE 4096
#endif

/** @brief Max cached arena chunks across all tiers (TLS free list limit). */
#ifndef CSILK_MAX_TLS_CHUNKS
#define CSILK_MAX_TLS_CHUNKS 16
#endif

/* Forward declarations for feature module driver types */
typedef struct csilk_cipher_driver csilk_cipher_driver_t;
typedef struct csilk_db_pool_s     csilk_db_pool_t;
typedef struct csilk_mq_s          csilk_mq_t;

/**
 * @brief Opaque request context type.
 * Created per-request by the server and passed to every handler and middleware.
 * Carries the request, response, path parameters, arena allocator, storage,
 * and connection state (WebSocket/SSE). Not thread-safe — access only from the
 * event-loop thread that owns the connection.
 */
typedef struct csilk_ctx_s csilk_ctx_t;

/**
 * @brief Pluggable storage driver for context key-value pairs.
 *
 * Allows users to replace the default in-memory arena-backed store with a
 * custom backend (e.g., a thread-local or external cache). Every function
 * receives the owning csilk_ctx_t so drivers can access per-request state.
 *
 * @note All driver functions are called from the I/O event-loop thread;
 *       implementations need not be thread-safe.
 */
typedef struct {
    /** @brief Store a value associated with @p key.
   *  @param c  Owning request context.
   *  @param key  NUL-terminated key string (copied internally).
   *  @param value  Opaque pointer to store. Ownership remains with caller. */
    void (*set)(csilk_ctx_t* c, const char* key, void* value);
    /** @brief Retrieve a value by key.
   *  @param c  Owning request context.
   *  @param key  NUL-terminated key string.
   *  @return The stored pointer, or NULL if @p key was never set. */
    void* (*get)(csilk_ctx_t* c, const char* key);
    /** @brief Clear all stored key-value pairs.
   *  Called during csilk_ctx_cleanup to release references. */
    void (*clear)(csilk_ctx_t* c);

    /** @brief Store a string value with an optional TTL (useful for Redis).
   *  @param c        Owning request context.
   *  @param key      NUL-terminated key string.
   *  @param value    NUL-terminated value string.
   *  @param ttl_sec  Time-to-live in seconds (0 = no expiry).
   *  @return 0 on success, non-zero on failure. */
    int (*set_string)(csilk_ctx_t* c, const char* key, const char* value, int ttl_sec);

    /** @brief Retrieve a string value by key.
   *  @param c    Owning request context.
   *  @param key  NUL-terminated key string.
   *  @return Heap-allocated string value (caller must free), or NULL if not found. */
    char* (*get_string)(csilk_ctx_t* c, const char* key);

    /** @brief Increment a numeric value by 1 with an optional TTL.
   *  @param c        Owning request context.
   *  @param key      NUL-terminated key string.
   *  @param ttl_sec  Time-to-live in seconds (set only if key is newly created; 0 = no expiry).
   *  @return The new value after incrementing, or -1 on error. */
    long long (*incr)(csilk_ctx_t* c, const char* key, int ttl_sec);

    /** @brief Atomic distributed cluster state synchronization (Circuit Breaker / Sliding Rate Limiter).
   *  @param c        Owning request context.
   *  @param key      NUL-terminated state key string.
   *  @param state    Current integer state value (e.g. 0=CLOSED, 1=OPEN, 2=HALF_OPEN).
   *  @param ttl_sec  Time-to-live in seconds for state eviction (0 = no expiry).
   *  @return 0 on successful synchronization, non-zero on error. */
    int (*sync_state)(csilk_ctx_t* c, const char* key, int state, int ttl_sec);
} csilk_storage_driver_t;

/**
 * @brief Function pointer for route handlers and middleware.
 *
 * Every handler receives the per-request context and operates on it
 * (reading request data, setting response data, calling csilk_next to
 * pass control to the next handler in the chain, etc.).
 *
 * @param c  The per-request context.
 */
typedef void (*csilk_handler_t)(csilk_ctx_t* c);

/**
 * @brief Interned IDs for well-known standard HTTP header names.
 *
 * Enables O(1) integer-based direct array indexing and header routing without
 * string comparison or hash computation overhead.
 */
typedef enum {
    CSILK_HDR_UNKNOWN = 0,
    CSILK_HDR_HOST,                     /**< Host */
    CSILK_HDR_CONTENT_TYPE,             /**< Content-Type */
    CSILK_HDR_CONTENT_LENGTH,           /**< Content-Length */
    CSILK_HDR_AUTHORIZATION,            /**< Authorization */
    CSILK_HDR_COOKIE,                   /**< Cookie */
    CSILK_HDR_SET_COOKIE,               /**< Set-Cookie */
    CSILK_HDR_ACCEPT,                   /**< Accept */
    CSILK_HDR_ACCEPT_ENCODING,          /**< Accept-Encoding */
    CSILK_HDR_ACCEPT_LANGUAGE,          /**< Accept-Language */
    CSILK_HDR_USER_AGENT,               /**< User-Agent */
    CSILK_HDR_CONNECTION,               /**< Connection */
    CSILK_HDR_UPGRADE,                  /**< Upgrade */
    CSILK_HDR_CACHE_CONTROL,            /**< Cache-Control */
    CSILK_HDR_ORIGIN,                   /**< Origin */
    CSILK_HDR_REFERER,                  /**< Referer */
    CSILK_HDR_SEC_WEBSOCKET_KEY,        /**< Sec-WebSocket-Key */
    CSILK_HDR_SEC_WEBSOCKET_VERSION,    /**< Sec-WebSocket-Version */
    CSILK_HDR_SEC_WEBSOCKET_EXTENSIONS, /**< Sec-WebSocket-Extensions */
    CSILK_HDR_SEC_WEBSOCKET_PROTOCOL,   /**< Sec-WebSocket-Protocol */
    CSILK_HDR_TRANSFER_ENCODING,        /**< Transfer-Encoding */
    CSILK_HDR_LOCATION,                 /**< Location */
    CSILK_HDR_IF_MODIFIED_SINCE,        /**< If-Modified-Since */
    CSILK_HDR_IF_NONE_MATCH,            /**< If-None-Match */
    CSILK_HDR_ETAG,                     /**< ETag */
    CSILK_HDR_SERVER,                   /**< Server */
    CSILK_HDR_DATE,                     /**< Date */
    CSILK_HDR_VARY,                     /**< Vary */
    CSILK_HDR_X_REQUEST_ID,             /**< X-Request-ID */
    CSILK_HDR_X_FORWARDED_FOR,          /**< X-Forwarded-For */
    CSILK_HDR_X_REAL_IP,                /**< X-Real-IP */
    CSILK_HDR_CONTENT_ENCODING,         /**< Content-Encoding */
    CSILK_HDR_MAX_KNOWN = 32
} csilk_header_id_t;

/** @brief Opaque header map type. */
typedef struct csilk_header_map_s csilk_header_map_t;

/** @brief Opaque request type. */
typedef struct csilk_request_s csilk_request_t;

/** @brief Opaque response type. */
typedef struct csilk_response_s csilk_response_t;

/** @brief Opaque path parameter type. */
typedef struct csilk_param_s csilk_param_t;

/** @brief Header iteration callback function.
 *
 * @param key    The header or parameter name.
 * @param value  The header or parameter value.
 * @param arg    User-provided closure argument.
 * @return 1 to continue iteration, 0 to stop early.
 */
typedef int (*csilk_header_cb)(const char* key, const char* value, void* arg);

/**
 * @brief Opaque arena allocator type.
 *
 * Provides bump-allocation semantics: memory is allocated in large chunks
 * and individual allocations are never freed — the entire arena is reset
 * or freed at once.  Ideal for request-scoped allocations because it is
 * faster than malloc/free and produces zero fragmentation.
 *
 * @note Not thread-safe — each request should have its own arena.
 */
typedef struct csilk_arena_s csilk_arena_t;

/**
 * @brief Log severity levels.
 *
 * Levels are ordered: messages at or above the configured minimum level are
 * emitted.  CSILK_LOG_FATAL terminates the process after logging.
 */
typedef enum {
    CSILK_LOG_TRACE, /**< Finest-grained diagnostic messages (development only).
                    */
    CSILK_LOG_DEBUG, /**< Debugging information useful during development. */
    CSILK_LOG_INFO,  /**< Normal operational messages (e.g., request completed). */
    CSILK_LOG_WARN,  /**< Warning conditions that are not errors (e.g., slow
                     request). */
    CSILK_LOG_ERROR, /**< Error conditions that still allow the server to
                      continue. */
    CSILK_LOG_FATAL  /**< Fatal errors; the server will exit after logging. */
} csilk_log_level_t;

/**
 * @brief Behavior when the asynchronous log queue is full.
 */
typedef enum {
    CSILK_LOG_OVERFLOW_DROP = 0,    /**< Drop new messages when queue is full. */
    CSILK_LOG_OVERFLOW_BLOCK = 1,   /**< Block/yield until space is available. */
    CSILK_LOG_OVERFLOW_FALLBACK = 2 /**< Write synchronously to stderr on overflow. */
} csilk_log_overflow_t;

/**
 * @brief Logger initialisation configuration.
 *
 * Controls log output destination, formatting, level filtering, and rotation.
 * Passed by value (not pointer) to csilk_log_init.
 */
typedef struct {
    csilk_log_level_t level; /**< Minimum level to emit (messages below this are filtered out). */
    const char*       file_path; /**< Path to the log file, or NULL to log to stderr. */
    size_t
        max_file_size; /**< Maximum file size in bytes before rotation (0 = rotation disabled). Requires @p file_path to be set. */
    int use_colors; /**< Enable ANSI colour escape codes: 1 = on, 0 = off, -1 = auto-detect (default). */
    int json_format; /**< When non-zero, emit newline-delimited JSON records instead of human-readable lines. */
    csilk_log_overflow_t
        overflow_strategy; /**< Behavior when async queue is full (default: CSILK_LOG_OVERFLOW_DROP). */
    size_t queue_capacity; /**< Preallocated node pool capacity (0 = default 8192). */
} csilk_log_config_t;

/**
 * @brief CORS middleware configuration.
 *
 * Maps directly to the Access-Control-* response headers.  Strings are used
 * as-is — the caller must ensure they remain valid for the lifetime of the
 * middleware.
 */
typedef struct {
    const char* allow_origin;      /**< Value of the Access-Control-Allow-Origin header
                               (e.g., "*" or "https://example.com"). */
    const char* allow_methods;     /**< Value of the Access-Control-Allow-Methods
                                 header (e.g., "GET, POST, PUT, DELETE"). */
    const char* allow_headers;     /**< Value of the Access-Control-Allow-Headers
                                 header (e.g., "Content-Type, Authorization"). */
    int         allow_credentials; /**< Non-zero to include
                                Access-Control-Allow-Credentials: true. */
    int         max_age;           /**< Value of Access-Control-Max-Age in seconds (e.g., 86400 for
                  24 h). */
} csilk_cors_config_t;

/**
 * @brief Security layer statistics.
 */
typedef struct {
    uint64_t rate_limit_blocks; /**< Total requests blocked by rate limiter. */
    uint64_t csrf_violations;   /**< Total CSRF token validation failures. */
    uint64_t auth_failures;     /**< Total failed authentication attempts. */
} csilk_security_stats_t;

/**
 * @brief OS-level process statistics.
 */
typedef struct {
    size_t rss_bytes;         /**< Resident Set Size memory in bytes. */
    double cpu_user_time_sec; /**< CPU time spent in user mode. */
    double cpu_sys_time_sec;  /**< CPU time spent in kernel mode. */
} csilk_process_stats_t;

/**
 * @brief Authentication validator callback.
 *
 * Receives the token extracted from the Authorization header and returns
 * non-zero if the token is valid.
 *
 * @param token The bearer token string extracted from the request.
 * @return Non-zero if the token is valid, 0 to reject.
 */
typedef int (*csilk_auth_validator_t)(const char* token);

/**
 * @brief A single validation rule for request parameter checking.
 *
 * Rules are collected into a NULL-terminated array and passed to
 * csilk_validate.  Each rule specifies constraints for one field.
 */
typedef struct {
    const char* field;  /**< Name of the field to validate. */
    int         flags;  /**< Bitwise OR of CSILK_VALID_* flags.  Set to 0 for no
                        constraints (only min/max apply). */
    int         min;    /**< Minimum allowed length (string fields) or numeric value (int
              fields). */
    int         max;    /**< Maximum allowed length (string fields) or numeric value (int
              fields). */
    const char* source; /**< Location to look for the field: "query", "form",
                         "header", "cookie", or NULL to auto-detect. */
} csilk_valid_rule_t;

/**
 * @brief A single part parsed from a multipart/form-data request body.
 *
 * Contains the field name, optional filename (for file uploads), content
 * type, and the binary data.  Strings are NUL-terminated fixed-size buffers;
 * data longer than the buffer is truncated.
 */
typedef struct {
    char         name[128];        /**< Form field name (NUL-terminated).  Truncated to 127
                         chars. */
    char         filename[256];    /**< Original filename for file uploads (empty string if
                         not a file).  Truncated to 255 chars. */
    char         content_type[64]; /**< Content-Type of the part (e.g., "image/png").
                            Truncated to 63 chars. */
    uint8_t*     data;             /**< Pointer to the part's binary data.  Valid until
                            csilk_ctx_cleanup. */
    size_t       data_len;         /**< Byte length of @p data. */
    csilk_ctx_t* ctx;              /**< Owning request context (for memory allocation). */
} csilk_multipart_part_t;

/**
 * @brief Callback invoked for each part during multipart parsing.
 *
 * @param part The parsed part.  The data pointer is valid only during the
 *             callback invocation — do not store the pointer for later use
 *             (copy the data if needed).
 */
typedef void (*csilk_multipart_handler_t)(csilk_multipart_part_t* part);

/* --- Forward declarations for key types defined in other modules --- */

/** @brief Opaque router node type.
 *  Nodes form a SIMD-accelerated segment-based prefix trie for efficient path
 *  matching. */
typedef struct csilk_router_node_s csilk_router_node_t;

/** @brief The main HTTP router. */
typedef struct csilk_router_s csilk_router_t;

/** @brief Route group structure. */
typedef struct csilk_group_s csilk_group_t;

/** @brief Main Server structure. */
typedef struct csilk_server_s csilk_server_t;

/** @brief Server configuration. */
typedef struct csilk_server_config_s csilk_server_config_t;

/** @brief Opaque Message Queue (event bus) instance. */
typedef struct csilk_mq_s csilk_mq_t;

/** @brief Opaque Message Queue context. */
typedef struct csilk_mq_ctx_s csilk_mq_ctx_t;

/**
 * @brief Memory ownership and lifecycle management taxonomy.
 *
 * Defines explicit ownership semantics across context, request/response bodies,
 * zero-copy views, pools, caches, and internal resources.
 */
/**
 * @brief Unified memory ownership state for HTTP request/response bodies.
 *
 * Defines exact cleanup and release semantics for memory associated with
 * bodies. Eliminates ambiguous flags (body_is_managed) in favor of a single
 * deterministic state machine.
 */
typedef enum {
    CSILK_OWN_NONE = 0, /**< No body attached, or released / uninitialized (0 bytes). */
    CSILK_OWN_BORROWED =
        1, /**< Borrowed view; caller/external holds buffer; framework does NOT free or copy. */
    CSILK_OWN_ARENA =
        2, /**< Memory allocated from request arena; freed automatically on arena reset. */
    CSILK_OWN_HEAP =
        3, /**< Explicitly heap-allocated buffer (malloc); freed via free() on release. */
    CSILK_OWN_POOL =
        4, /**< Buffer from size-class pool; returned to pool via csilk_body_free(ptr, capacity). */
    CSILK_OWN_TRANSFER = 5, /**< Ownership transferred to context; freed via free() on release. */
    /* Backward compatibility aliases */
    CSILK_OWN_OWNED = CSILK_OWN_HEAP,
    CSILK_OWN_TLS_CACHE = CSILK_OWN_POOL,
    CSILK_OWN_SHARED = CSILK_OWN_BORROWED
} csilk_ownership_t;

/**
 * @brief Convert ownership enum value to human-readable string.
 *
 * @param ownership Ownership enum value.
 * @return Static string representation ("NONE", "BORROWED", "ARENA", "HEAP", "POOL", "TRANSFER", "UNKNOWN").
 */
static inline const char*
csilk_ownership_str(csilk_ownership_t ownership)
{
    switch (ownership) {
    case CSILK_OWN_NONE:
        return "NONE";
    case CSILK_OWN_BORROWED:
        return "BORROWED";
    case CSILK_OWN_ARENA:
        return "ARENA";
    case CSILK_OWN_HEAP:
        return "HEAP";
    case CSILK_OWN_POOL:
        return "POOL";
    case CSILK_OWN_TRANSFER:
        return "TRANSFER";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Zero-copy read-only string/byte slice view.

 *
 * References external memory without allocation. Used by the HTTP parser
 * and high-performance accessor APIs to reference fields directly in the
 * receive buffer or arena without copying.
 *
 * @note The data/ptr is a borrowed slice and is NOT guaranteed to be
 *       NUL-terminated. It is valid only for the lifetime of the underlying
 *       network buffer or request context.
 */
typedef struct csilk_view_s {
    union {
        const char* ptr;  /**< Pointer to slice data (not NUL-terminated). */
        const char* data; /**< Alias for backwards compatibility. */
    };
    size_t len;           /**< Length of the slice in bytes. */
} csilk_view_t;

/** @brief Backward compatibility alias for csilk_view_t. */
typedef csilk_view_t csilk_str_view_t;

/**
 * @brief Construct a string view from a pointer and length.
 *
 * @param ptr Pointer to data (may be non-NUL-terminated).
 * @param len Length in bytes.
 * @return A csilk_view_t slice.
 */
static inline csilk_view_t
csilk_view(const char* ptr, size_t len)
{
    csilk_view_t v;
    v.ptr = ptr;
    v.len = len;
    return v;
}

/**
 * @brief Construct a string view from a NUL-terminated C string.
 *
 * @param str NUL-terminated C string (or NULL).
 * @return A csilk_view_t slice (empty view if str is NULL).
 */
static inline csilk_view_t
csilk_view_from_str(const char* str)
{
    if (!str) {
        return csilk_view(NULL, 0);
    }
    return csilk_view(str, strlen(str));
}

/**
 * @brief Check if a string view is empty or NULL.
 *
 * @param view The view to check.
 * @return 1 if view.ptr is NULL or view.len is 0, 0 otherwise.
 */
static inline int
csilk_view_is_empty(csilk_view_t view)
{
    return (!view.ptr || view.len == 0);
}

/**
 * @brief Compare a view against a NUL-terminated C string (case-sensitive).
 *
 * @param view The view to compare.
 * @param str  NUL-terminated C string.
 * @return 0 if equal, negative if view < str, positive if view > str.
 */
int csilk_view_cmp(csilk_view_t view, const char* str);

/**
 * @brief Compare a view against a NUL-terminated C string (case-insensitive).
 *
 * @param view The view to compare.
 * @param str  NUL-terminated C string.
 * @return 0 if equal, negative if view < str, positive if view > str.
 */
int csilk_view_casecmp(csilk_view_t view, const char* str);

/**
 * @brief Compare two views for equality (exact length and byte match).
 *
 * @param a First view.
 * @param b Second view.
 * @return 1 if identical, 0 otherwise.
 */
int csilk_view_equal(csilk_view_t a, csilk_view_t b);

/**
 * @brief Materialize a view into a NUL-terminated string allocated in the request arena.
 *
 * Safe for the entire request lifecycle without manual free.
 *
 * @param c    The request context.
 * @param view The view to persist.
 * @return NUL-terminated arena string, or NULL on error.
 */
const char* csilk_view_to_arena(csilk_ctx_t* c, csilk_view_t view);

/**
 * @brief Materialize a view into a newly allocated NUL-terminated heap string.
 *
 * Caller must free with csilk_free() or free().
 *
 * @param view The view to copy.
 * @return Newly allocated NUL-terminated string, or NULL on error.
 */
char* csilk_view_to_heap(csilk_view_t view);

/**
 * @brief Convert a string view to a null-terminated heap-allocated string.
 *
 * Copies the contents of the string view into a newly allocated buffer.
 * Caller must free the returned pointer with csilk_free().
 *
 * @param view The string view to copy.
 * @return A newly allocated null-terminated string, or NULL on failure.
 */
char* csilk_str_view_to_string(const csilk_str_view_t* view);

/**
 * @brief Persist a string view into arena memory.
 *
 * Copies the contents of the string view into the request arena.
 * The returned pointer is valid until the request completes (arena reset).
 *
 * @param c    The request context (for arena access).
 * @param view The string view to persist.
 * @return A null-terminated string in arena memory, or NULL on failure.
 */
const char* csilk_str_view_persist(csilk_ctx_t* c, const csilk_str_view_t* view);

void* csilk_malloc(size_t size);
void  csilk_free(void* ptr);
char* csilk_strdup(const char* s);
