/**
 * @file types.h
 * @brief Core data types for the csilk HTTP web framework.
 *
 * Defines all public structs, typedefs, enums, and key constants
 * needed by the request context, router, server, middleware,
 * WebSocket, SSE, and utility APIs.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_TYPES_H
#define CSILK_TYPES_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "cJSON.h"

#include "csilk/errors.h"
#include "csilk/version.h"

/** @brief Internal visibility macro for symbols not part of the public API. */
#if defined(__GNUC__) && __GNUC__ >= 4
#define CSILK_INTERNAL __attribute__((visibility("hidden")))
#else
#define CSILK_INTERNAL
#endif

/* Forward declarations to break circular dependency:
   csilk/drivers/db.h includes csilk.h, so types defined in db.h
   must be forward-declared here before the include. */
typedef struct csilk_db_pool_s csilk_db_pool_t;

#include "csilk/drivers/ai.h"
#include "csilk/drivers/cipher.h"
#include "csilk/drivers/perm.h"
#include "csilk/reflection/reflect.h"

/**
 * @brief Maximum number of URL path parameters that can be extracted from a
 * single request.  Parameters beyond this limit are silently ignored.
 * Tune if your routes contain more than 20 dynamic segments.
 */
static constexpr int CSILK_MAX_PARAMS = 20;

/**
 * @brief Maximum number of items that can be stored in the context key-value storage.
 *
 * This limit prevents uncontrolled memory consumption in the request arena
 * by preventing a single request from setting an excessive number of keys.
 */
static constexpr int CSILK_MAX_STORAGE = 64;

/**
 * @brief Opaque request context type.
 * Created per-request by the server and passed to every handler and middleware.
 * Carries the request, response, path parameters, arena allocator, storage,
 * and connection state (WebSocket/SSE). Not thread-safe — access only from the
 * libuv loop thread that owns the connection.
 */
typedef struct csilk_ctx_s csilk_ctx_t;

/**
 * @brief Pluggable storage driver for context key-value pairs.
 *
 * Allows users to replace the default in-memory arena-backed store with a
 * custom backend (e.g., a thread-local or external cache). Every function
 * receives the owning csilk_ctx_t so drivers can access per-request state.
 *
 * @note All driver functions are called from the libuv event-loop thread;
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
   *  @return The stored pointer, or nullptr if @p key was never set. */
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
   *  @return Heap-allocated string value (caller must free), or nullptr if not found. */
	char* (*get_string)(csilk_ctx_t* c, const char* key);

	/** @brief Increment a numeric value by 1 with an optional TTL.
   *  @param c        Owning request context.
   *  @param key      NUL-terminated key string.
   *  @param ttl_sec  Time-to-live in seconds (set only if key is newly created; 0 = no expiry).
   *  @return The new value after incrementing, or -1 on error. */
	long long (*incr)(csilk_ctx_t* c, const char* key, int ttl_sec);
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
	CSILK_LOG_INFO,	 /**< Normal operational messages (e.g., request completed). */
	CSILK_LOG_WARN,	 /**< Warning conditions that are not errors (e.g., slow
                     request). */
	CSILK_LOG_ERROR, /**< Error conditions that still allow the server to
                      continue. */
	CSILK_LOG_FATAL	 /**< Fatal errors; the server will exit after logging. */
} csilk_log_level_t;

/**
 * @brief Logger initialisation configuration.
 *
 * Controls log output destination, formatting, level filtering, and rotation.
 * Passed by value (not pointer) to csilk_log_init.
 */
typedef struct {
	csilk_log_level_t level; /**< Minimum level to emit (messages below this are
                              filtered out). */
	const char* file_path;	 /**< Path to the log file, or nullptr to log to stderr. */
	size_t max_file_size;	 /**< Maximum file size in bytes before rotation (0 =
                        rotation disabled). Requires @p file_path to be set. */
	int use_colors;		 /**< Enable ANSI colour escape codes: 1 = on, 0 = off, -1 =
                        auto-detect (default). */
	int json_format;	 /**< When non-zero, emit newline-delimited JSON records
                        instead of human-readable lines. */
} csilk_log_config_t;

/**
 * @brief CORS middleware configuration.
 *
 * Maps directly to the Access-Control-* response headers.  Strings are used
 * as-is — the caller must ensure they remain valid for the lifetime of the
 * middleware.
 */
typedef struct {
	const char* allow_origin;  /**< Value of the Access-Control-Allow-Origin header
                               (e.g., "*" or "https://example.com"). */
	const char* allow_methods; /**< Value of the Access-Control-Allow-Methods
                                 header (e.g., "GET, POST, PUT, DELETE"). */
	const char* allow_headers; /**< Value of the Access-Control-Allow-Headers
                                 header (e.g., "Content-Type, Authorization"). */
	int allow_credentials;	   /**< Non-zero to include
                                Access-Control-Allow-Credentials: true. */
	int max_age;		   /**< Value of Access-Control-Max-Age in seconds (e.g., 86400 for
                  24 h). */
} csilk_cors_config_t;

/**
 * @brief Security layer statistics.
 */
typedef struct {
	uint64_t rate_limit_blocks; /**< Total requests blocked by rate limiter. */
	uint64_t csrf_violations;   /**< Total CSRF token validation failures. */
	uint64_t auth_failures;	    /**< Total failed authentication attempts. */
} csilk_security_stats_t;

/**
 * @brief OS-level process statistics.
 */
typedef struct {
	size_t rss_bytes;	  /**< Resident Set Size memory in bytes. */
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
 * Rules are collected into a nullptr-terminated array and passed to
 * csilk_validate.  Each rule specifies constraints for one field.
 */
typedef struct {
	const char* field;  /**< Name of the field to validate. */
	int flags;	    /**< Bitwise OR of CSILK_VALID_* flags.  Set to 0 for no
                        constraints (only min/max apply). */
	int min;	    /**< Minimum allowed length (string fields) or numeric value (int
              fields). */
	int max;	    /**< Maximum allowed length (string fields) or numeric value (int
              fields). */
	const char* source; /**< Location to look for the field: "query", "form",
                         "header", "cookie", or nullptr to auto-detect. */
} csilk_valid_rule_t;

/**
 * @brief A single part parsed from a multipart/form-data request body.
 *
 * Contains the field name, optional filename (for file uploads), content
 * type, and the binary data.  Strings are NUL-terminated fixed-size buffers;
 * data longer than the buffer is truncated.
 */
typedef struct {
	char name[128];	       /**< Form field name (NUL-terminated).  Truncated to 127
                         chars. */
	char filename[256];    /**< Original filename for file uploads (empty string if
                         not a file).  Truncated to 255 chars. */
	char content_type[64]; /**< Content-Type of the part (e.g., "image/png").
                            Truncated to 63 chars. */
	uint8_t* data;	       /**< Pointer to the part's binary data.  Valid until
                            csilk_ctx_cleanup. */
	size_t data_len;       /**< Byte length of @p data. */
	csilk_ctx_t* ctx;      /**< Owning request context (for memory allocation). */
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
 *  Nodes form a compressed radix tree (Patricia trie) for efficient path
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

/* --- Crypto Driver Interface --- */

/**
 * @brief Pluggable cryptographic primitive driver.
 *
 * Allows users to replace the default software implementations of SHA256,
 * HMAC-SHA256, and UUID generation (e.g., with hardware-accelerated or
 * FIPS-compliant versions).  All function pointers must be non-nullptr.
 */
typedef struct {
	/** @brief Compute the SHA-256 hash of a buffer.
   *  @param data  Input data.
   *  @param len   Input length.
   *  @param[out] out  32-byte hash output. */
	void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
	/** @brief Compute HMAC-SHA256.
   *  @param key       HMAC key.
   *  @param key_len   Key length.
   *  @param data      Input data.
   *  @param data_len  Input length.
   *  @param[out] out  32-byte HMAC output. */
	void (*hmac_sha256)(const uint8_t* key,
			    size_t key_len,
			    const uint8_t* data,
			    size_t data_len,
			    uint8_t out[32]);
	/** @brief Generate a random version-4 UUID string.
   *  @param[out] buf  Output buffer of at least 37 bytes.  Populated with a
   *                   NUL-terminated UUID string. */
	void (*generate_uuid)(char buf[37]);
	/** @brief Fill a buffer with cryptographically secure random bytes.
   *  @param[out] out  Buffer to fill.
   *  @param      len  Number of bytes to generate.
   *  @return 0 on success, -1 on failure. */
	int (*fill_random)(void* out, size_t len);
} csilk_crypto_driver_t;

/* Include db.h last — it pulls in csilk/csilk.h and must see all core
   types already defined to avoid circular-definition errors. */
#include "csilk/drivers/db.h"

void* csilk_malloc(size_t size);
void csilk_free(void* ptr);
char* csilk_strdup(const char* s);

#endif /* CSILK_TYPES_H */
