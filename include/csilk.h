/**
 * @file csilk.h
 * @brief High-performance C web framework — main public API header.
 *
 * Defines all public types, enums, macros, and function declarations
 * for the csilk HTTP web framework, including the request context,
 * router, server, middleware, WebSocket, SSE, and utility APIs.
 * Inspired by Gin (Golang).
 * @version 0.2.1
 * @copyright MIT License
 */

#ifndef CSILK_H
#define CSILK_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "cJSON.h"
#include "csilk_db.h"
#include "csilk_reflect.h"

/** @brief Csilk framework version. */
#define CSILK_VERSION "0.2.1"
/** @brief Maximum number of URL parameters. */
#define CSILK_MAX_PARAMS 20

/** @name HTTP Status Codes
 *  Standardized macros for common HTTP response status codes.
 *  @{ */
#define CSILK_STATUS_CONTINUE 100
#define CSILK_STATUS_SWITCHING_PROTOCOLS 101
#define CSILK_STATUS_OK 200
#define CSILK_STATUS_CREATED 201
#define CSILK_STATUS_NO_CONTENT 204
#define CSILK_STATUS_PARTIAL_CONTENT 206
#define CSILK_STATUS_MOVED_PERMANENTLY 301
#define CSILK_STATUS_FOUND 302
#define CSILK_STATUS_NOT_MODIFIED 304
#define CSILK_STATUS_TEMPORARY_REDIRECT 307
#define CSILK_STATUS_BAD_REQUEST 400
#define CSILK_STATUS_UNAUTHORIZED 401
#define CSILK_STATUS_PAYMENT_REQUIRED 402
#define CSILK_STATUS_FORBIDDEN 403
#define CSILK_STATUS_NOT_FOUND 404
#define CSILK_STATUS_METHOD_NOT_ALLOWED 405
#define CSILK_STATUS_REQUEST_TIMEOUT 408
#define CSILK_STATUS_CONFLICT 409
#define CSILK_STATUS_GONE 410
#define CSILK_STATUS_PAYLOAD_TOO_LARGE 413
#define CSILK_STATUS_RANGE_NOT_SATISFIABLE 416
#define CSILK_STATUS_URI_TOO_LONG 414
#define CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE 415
#define CSILK_STATUS_TOO_MANY_REQUESTS 429
#define CSILK_STATUS_INTERNAL_SERVER_ERROR 500
#define CSILK_STATUS_NOT_IMPLEMENTED 501
#define CSILK_STATUS_BAD_GATEWAY 502
#define CSILK_STATUS_SERVICE_UNAVAILABLE 503
#define CSILK_STATUS_GATEWAY_TIMEOUT 504
/** @} */

/** @brief Opaque request context type. */
typedef struct csilk_ctx_s csilk_ctx_t;

/** @brief Storage driver interface for context key-value storage. */
typedef struct {
  /** @brief Store a value by key. */
  void (*set)(csilk_ctx_t* c, const char* key, void* value);
  /** @brief Retrieve a value by key. */
  void* (*get)(csilk_ctx_t* c, const char* key);
  /** @brief Clear all stored items (called during ctx cleanup). */
  void (*clear)(csilk_ctx_t* c);
} csilk_storage_driver_t;

/** @brief Handler function pointer type. */
typedef void (*csilk_handler_t)(csilk_ctx_t* c);

/** @brief HTTP Header structure (linked list node for hash table). */
typedef struct csilk_header_s {
  char* key;                   /**< Header field name. */
  char* value;                 /**< Header field value. */
  size_t key_len;              /**< Cached strlen(key). */
  size_t value_len;            /**< Cached strlen(value). */
  struct csilk_header_s* next; /**< Pointer to next header in bucket. */
} csilk_header_t;

/** @brief Number of buckets in header hash table.
 * Override at compile-time with -DCSILK_HEADER_BUCKETS=N for your workload. */
#ifndef CSILK_HEADER_BUCKETS
#define CSILK_HEADER_BUCKETS 64
#endif

/** @brief Header hash table structure. */
typedef struct csilk_header_map_s {
  csilk_header_t* buckets[CSILK_HEADER_BUCKETS];
} csilk_header_map_t;

/** @brief HTTP Request structure. */
typedef struct {
  char* method;                    /**< HTTP method (e.g., "GET"). */
  char* path;                      /**< Decoded URL path. */
  char* body;                      /**< Raw request body. */
  size_t body_len;                 /**< Length of the request body. */
  csilk_header_map_t headers;      /**< Hash map of request headers. */
  csilk_header_map_t query_params; /**< Hash map of query parameters. */
  csilk_header_map_t form_params;  /**< Hash map of form urlencoded fields. */
} csilk_request_t;

/** @brief HTTP Response structure. */
typedef struct {
  int status;                 /**< HTTP status code. */
  const char* body;           /**< Response body content. */
  size_t body_len;            /**< Length of the response body. */
  csilk_header_map_t headers; /**< Hash map of response headers. */
  int body_is_managed;        /**< Flag if body is managed by free(). */
} csilk_response_t;

/** @brief URL path parameter. */
typedef struct {
  char* key;   /**< Parameter key (from route, e.g., "id"). */
  char* value; /**< Actual parameter value from URL. */
} csilk_param_t;

/** @brief Opaque arena allocator type. */
typedef struct csilk_arena_s csilk_arena_t;

/** @brief Get the request HTTP method (e.g., "GET"). */
const char* csilk_get_method(csilk_ctx_t* c);

/** @brief Get the request URL path. */
const char* csilk_get_path(csilk_ctx_t* c);

/** @brief Get the request body and its length.
 * @param c The request context.
 * @param out_len Optional pointer to store body length.
 * @return Pointer to raw body data (NULL if no body). */
const char* csilk_get_body(csilk_ctx_t* c, size_t* out_len);

/** @brief Get the request body length. */
size_t csilk_get_body_len(csilk_ctx_t* c);

/** @brief Check if the connection has been upgraded to WebSocket. */
int csilk_is_websocket(csilk_ctx_t* c);

/** @brief Check if the connection is a Server-Sent Events stream. */
int csilk_is_sse(csilk_ctx_t* c);

/** @brief Check if the handler chain has been aborted. */
int csilk_is_aborted(csilk_ctx_t* c);

/** @brief Set the callback for incoming WebSocket messages. */
void csilk_set_on_ws_message(csilk_ctx_t* c,
                             void (*cb)(csilk_ctx_t* c, const uint8_t* payload,
                                        size_t len, int opcode));

/** @brief Get the unique request ID.
 * @param c The request context.
 * @return The request ID string (UUID format). */
const char* csilk_get_request_id(csilk_ctx_t* c);

/** @brief Get the arena allocator associated with the context.
 * @param c The request context.
 * @return Pointer to the arena allocator. */
csilk_arena_t* csilk_get_arena(csilk_ctx_t* c);

/** @brief Get the response status code.
 * @param c The request context.
 * @return The HTTP status code. */
int csilk_get_status(csilk_ctx_t* c);

/** @brief Set whether the response will be sent asynchronously.
 * @param c The request context.
 * @param is_async 1 to enable async mode, 0 to disable. */
void csilk_set_async(csilk_ctx_t* c, int is_async);

/** @brief Check if the response is in async mode.
 * @param c The request context.
 * @return 1 if async, 0 otherwise. */
int csilk_is_async(csilk_ctx_t* c);

/** @brief Get the response body.
 * @param c The request context.
 * @param out_len Optional pointer to store body length.
 * @return Pointer to the response body data. */
const char* csilk_get_response_body(csilk_ctx_t* c, size_t* out_len);

/** @brief Set the response body directly.
 * Useful for middleware to modify the response (e.g., compression).
 * @param c The request context.
 * @param body Pointer to body data.
 * @param len Body length.
 * @param managed If 1, the framework will free() the body when finished. */
void csilk_set_response_body(csilk_ctx_t* c, const char* body, size_t len,
                             int managed);

/** @brief Redirect to another URL with custom status.
 * @param c The request context.
 * @param status HTTP status code (e.g., 301, 302).
 * @param location The destination URL. */
void csilk_redirect(csilk_ctx_t* c, int status, const char* location);

/** @brief Redirect to another URL (302 Found).
 * @param c The request context.
 * @param location The destination URL. */
void csilk_redirect_simple(csilk_ctx_t* c, const char* location);

/** @brief Store a value in the context.
 * The key is duplicated and stored in the request arena.
 * Note: The context does NOT take ownership of the value pointer;
 * the caller is responsible for the value's lifetime.
 * @param c The request context.
 * @param key Item key name.
 * @param value Pointer to data. */
void csilk_set(csilk_ctx_t* c, const char* key, void* value);

/** @brief Retrieve a value from the context.
 * @param c The request context.
 * @param key Item key name.
 * @return Pointer to data, or NULL if not found. */
void* csilk_get(csilk_ctx_t* c, const char* key);

/** @brief Move to the next handler in the onion model chain.
 * @param c The request context. */
void csilk_next(csilk_ctx_t* c);

/** @brief Abort the handler chain execution immediately.
 * @param c The request context. */
void csilk_abort(csilk_ctx_t* c);

/** @brief Set the response status code.
 * @param c The request context.
 * @param status The HTTP status code (e.g., 200, 404). */
void csilk_status(csilk_ctx_t* c, int status);

/** @brief Set response body and status code (Plain text/String).
 * Memory is managed by the request arena.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param msg The response body message string. */
void csilk_string(csilk_ctx_t* c, int status, const char* msg);

/** @brief Get a URL path parameter by key.
 * @param c The request context.
 * @param key The parameter key name.
 * @return The parameter value string, or NULL if not found. */
const char* csilk_get_param(csilk_ctx_t* c, const char* key);

/** @brief Get a request header by key.
 * Case-insensitive lookup.
 * @param c The request context.
 * @param key The header field name.
 * @return The header value string, or NULL if not found. */
const char* csilk_get_header(csilk_ctx_t* c, const char* key);

/** @brief Get a response header by key.
 * Case-insensitive lookup.
 * @param c The request context.
 * @param key The header field name.
 * @return The header value string, or NULL if not found. */
const char* csilk_get_response_header(csilk_ctx_t* c, const char* key);

/** @brief Get a query parameter by key.
 * @param c The request context.
 * @param key The query parameter key name.
 * @return The query value string, or NULL if not found. */
const char* csilk_get_query(csilk_ctx_t* c, const char* key);

/** @brief Parse the request body as application/x-www-form-urlencoded.
 * Parsed fields are accessible via csilk_get_form_field().
 * @param c The request context. */
void csilk_parse_form_urlencoded(csilk_ctx_t* c);

/** @brief Get a form urlencoded field value by key.
 * @param c The request context.
 * @param key The form field key name.
 * @return The field value string, or NULL if not found. */
const char* csilk_get_form_field(csilk_ctx_t* c, const char* key);

/** @brief Set a request header.
 * @param c The request context.
 * @param key The header field name.
 * @param value The header field value string. */
void csilk_set_request_header(csilk_ctx_t* c, const char* key,
                              const char* value);

/** @brief Set a response header (overwrites if exists).
 * @param c The request context.
 * @param key The header field name.
 * @param value The header field value string. */
void csilk_set_header(csilk_ctx_t* c, const char* key, const char* value);

/** @brief Add a response header (allows multiple headers with same key).
 * @param c The request context.
 * @param key The header field name.
 * @param value The header field value string. */
void csilk_add_header(csilk_ctx_t* c, const char* key, const char* value);

/** @brief Clean up all context-related resources (arena, headers, etc.).
 * @param c The request context. */
void csilk_ctx_cleanup(csilk_ctx_t* c);

/** @brief Create a new request-scoped arena allocator.
 * @param default_chunk_size Initial size of the arena memory chunk.
 * @return Pointer to the new arena instance. */
csilk_arena_t* csilk_arena_new(size_t default_chunk_size);

/** @brief Allocate memory from the arena.
 * Memory will be freed automatically in csilk_ctx_cleanup.
 * @param arena The arena instance.
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure. */
void* csilk_arena_alloc(csilk_arena_t* arena, size_t size);

/** @brief Duplicate a string using the arena allocator.
 * @param arena The arena instance.
 * @param s Source string.
 * @return Allocated copy of the string. */
char* csilk_arena_strdup(csilk_arena_t* arena, const char* s);

/** @brief Explicitly free the arena and all its chunks.
 * @param arena The arena instance. */
void csilk_arena_free(csilk_arena_t* arena);

/** @brief Reset the arena for reuse without freeing chunks.
 * @param arena The arena instance. */
void csilk_arena_reset(csilk_arena_t* arena);

/** @brief Recovery middleware handler.
 * Prevents server from crashing on panics and returns 500.
 * @param c The request context. */
void csilk_recovery_handler(csilk_ctx_t* c);

/** @brief Trigger a panic in the current handler.
 * Will be caught by the nearest recovery middleware.
 * @param c The request context. */
void csilk_panic(csilk_ctx_t* c);

/** @brief Logging levels. */
typedef enum {
  CSILK_LOG_TRACE, /**< Trace-level logging. */
  CSILK_LOG_DEBUG, /**< Debug-level logging. */
  CSILK_LOG_INFO,  /**< Informational logging. */
  CSILK_LOG_WARN,  /**< Warning-level logging. */
  CSILK_LOG_ERROR, /**< Error-level logging. */
  CSILK_LOG_FATAL  /**< Fatal error logging. */
} csilk_log_level_t;

/** @brief Logger configuration. */
typedef struct {
  csilk_log_level_t level; /**< Minimum logging level. */
  const char* file_path;   /**< Log file path (NULL for stdout). */
  size_t max_file_size; /**< Max size before rotation (bytes, 0 to disable). */
  int use_colors;       /**< Enable ANSI colors (auto-detected if -1). */
  int json_format;      /**< Enable JSON structured log output. */
} csilk_log_config_t;

/** @brief Initialize the global logger with config.
 * @param config Logger configuration.
 * @return 0 on success, -1 on failure. */
int csilk_log_init(csilk_log_config_t config);

/** @brief Internal log function (use macros instead).
 * @param lv Log severity level.
 * @param file Source file name (__FILE__).
 * @param line Source line number (__LINE__).
 * @param func Function name (__func__).
 * @param fmt Printf-style format string.
 * @param ... Format arguments. */
void _csilk_log_internal(csilk_log_level_t lv, const char* file, int line,
                         const char* func, const char* fmt, ...);

/** @brief Close the global logger. */
void csilk_log_close();

/** @brief Log a structured JSON message with extra key-value fields.
 *
 * Produces a JSON log line with the standard fields
 * (time/level/file/line/func/msg) plus any fields in @p extra.  If json_format
 * is off this behaves like a normal log line (extra fields are ignored).
 *
 * @param lv    Log severity level.
 * @param file    Source file name (__FILE__).
 * @param line    Source line number (__LINE__).
 * @param func    Function name (__func__).
 * @param extra   cJSON object with extra structured fields (can be NULL).
 *                Ownership is taken — do not use after the call.
 * @param fmt     Printf-style format string for the log message.
 * @param ...     Format arguments. */
void _csilk_log_structured(csilk_log_level_t lv, const char* file, int line,
                           const char* func, cJSON* extra, const char* fmt,
                           ...);

/** @brief Check whether the logger is in JSON format mode.
 * @return 1 if json_format is enabled, 0 otherwise. */
int csilk_log_is_json(void);

/** @brief Set the Request ID for the current thread (for log correlation).
 * @param request_id The Request ID string, or NULL to clear. */
void csilk_log_set_request_id(const char* request_id);

/** @brief Create a simple key-value cJSON object for structured logging.
 *
 * Convenience helper that builds a cJSON object from alternating key/value
 * string pairs terminated by a NULL key.
 *
 * @code
 *   cJSON* fields = csilk_log_make_kv("method", method, "path", path, NULL);
 *   _csilk_log_structured(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, fields,
 *                         "request completed");
 * @endcode
 *
 * @param key   First key.
 * @param ...   Value, then key, value, ... terminated by NULL.
 * @return New cJSON object (caller owns). */
cJSON* csilk_log_make_kv(const char* key, ...);

/** @name Logging Macros
 * Convenience macros that capture source location.
 * @{ */
/** @brief Log a TRACE-level message. */
#define CSILK_LOG_T(...)                                             \
  _csilk_log_internal(CSILK_LOG_TRACE, __FILE__, __LINE__, __func__, \
                      __VA_ARGS__)
/** @brief Log a DEBUG-level message. */
#define CSILK_LOG_D(...)                                             \
  _csilk_log_internal(CSILK_LOG_DEBUG, __FILE__, __LINE__, __func__, \
                      __VA_ARGS__)
/** @brief Log an INFO-level message. */
#define CSILK_LOG_I(...) \
  _csilk_log_internal(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a WARN-level message. */
#define CSILK_LOG_W(...) \
  _csilk_log_internal(CSILK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log an ERROR-level message. */
#define CSILK_LOG_E(...)                                             \
  _csilk_log_internal(CSILK_LOG_ERROR, __FILE__, __LINE__, __func__, \
                      __VA_ARGS__)
/** @brief Log a FATAL-level message. */
#define CSILK_LOG_F(...)                                             \
  _csilk_log_internal(CSILK_LOG_FATAL, __FILE__, __LINE__, __func__, \
                      __VA_ARGS__)

/** @brief Log a structured JSON message (only meaningful when json_format is
 * on).
 * @param level Log level.
 * @param extra  cJSON* with extra fields (can be NULL).
 * @param ...    printf-style format and args for the message string. */
#define CSILK_LOG_STRUCT(level, extra, ...) \
  _csilk_log_structured(level, __FILE__, __LINE__, __func__, extra, __VA_ARGS__)
/** @} */

/** @brief Logging middleware handler.
 * Logs request method, path, and processing time.
 * @param c The request context. */
void csilk_logger_handler(csilk_ctx_t* c);

/** @brief Request ID middleware.
 * Generates a unique ID for each request and sets X-Request-Id header.
 * @param c The request context. */
void csilk_request_id_middleware(csilk_ctx_t* c);

/** @brief Built-in Health Check handler.
 * Returns a simple JSON response {"status": "up"}.
 * @param c The request context. */
void csilk_health_check_handler(csilk_ctx_t* c);

/** @brief CORS middleware configuration. */
typedef struct {
  const char* allow_origin;  /**< Access-Control-Allow-Origin. */
  const char* allow_methods; /**< Access-Control-Allow-Methods. */
  const char* allow_headers; /**< Access-Control-Allow-Headers. */
  int allow_credentials;     /**< Access-Control-Allow-Credentials. */
  int max_age;               /**< Access-Control-Max-Age. */
} csilk_cors_config_t;

/** @brief Enable CORS with specified configuration.
 * @param c The request context.
 * @param config CORS settings (pointer, for efficiency). */
void csilk_cors_middleware(csilk_ctx_t* c, const csilk_cors_config_t* config);

/** @brief Simple IP-based rate limiting middleware.
 * @param c The request context.
 * @param limit Maximum requests per minute. */
void csilk_rate_limit_middleware(csilk_ctx_t* c, int limit);

/** @brief Simple stateless CSRF protection middleware.
 * @param c The request context. */
void csilk_csrf_middleware(csilk_ctx_t* c);

/** @brief Generate a random CSRF token string.
 * @param buf Output buffer (at least 33 bytes for 32-char hex token).
 * @param buf_size Size of the output buffer.
 * @return 0 on success, -1 on error. */
int csilk_csrf_generate_token(char* buf, size_t buf_size);

/** @brief Server configuration options. */
typedef struct csilk_server_config_s {
  unsigned int idle_timeout_ms; /**< Keep-alive idle timeout (ms). */
  unsigned int
      read_timeout_ms; /**< Max time to read a request (ms, 0=disabled). */
  unsigned int
      write_timeout_ms; /**< Max time to write a response (ms, 0=disabled). */
  unsigned int request_timeout_ms; /**< Max time for full request/response cycle
                                      (ms, 0=disabled). */
  size_t max_body_size;            /**< Maximum request body size. */
  size_t max_header_size;          /**< Maximum total request headers size. */
  size_t max_url_size;             /**< Maximum URL length (0=disabled). */
  size_t max_headers_count; /**< Maximum number of headers (0=disabled). */
  int max_connections;      /**< Max concurrent connections (0=unlimited). */
  int listen_backlog;       /**< TCP listen backlog. */
  int tcp_nodelay;          /**< Enable TCP_NODELAY. */
  int tcp_keepalive;  /**< Enable TCP keep-alive (seconds, 0 to disable). */
  int worker_threads; /**< Number of worker threads (SO_REUSEPORT). */

  /* TLS configuration */
  int enable_tls;      /**< Enable HTTPS/TLS. */
  char* tls_cert_file; /**< Path to SSL certificate file. */
  char* tls_key_file;  /**< Path to SSL private key file. */
  char* tls_ca_file;   /**< Path to CA bundle file (optional). */
  int tls_verify_peer; /**< Require client certificate. */
} csilk_server_config_t;

/** @brief Global Configuration structure. */
typedef struct {
  int port;                     /**< Server port. */
  csilk_server_config_t server; /**< Server low-level config. */
  csilk_log_config_t logger;    /**< Logger config. */
  struct {
    int enable;                 /**< Enable CORS. */
    csilk_cors_config_t config; /**< CORS config. */
  } cors;                       /**< CORS settings. */
  struct {
    int enable;              /**< Enable rate limiting. */
    int requests_per_minute; /**< Limit per IP. */
  } rate_limit;              /**< Rate limit settings. */
  struct {
    int enable;     /**< Enable static file serving. */
    char* root_dir; /**< Local directory path. */
    char* prefix;   /**< URL prefix (e.g., "/static"). */
  } static_files;   /**< Static file settings. */
  struct {
    int enable_logger;   /**< Enable request logging middleware. */
    int enable_recovery; /**< Enable panic recovery middleware. */
    int enable_csrf;     /**< Enable CSRF protection middleware. */
    int enable_auth;     /**< Enable auth middleware. */
    char* auth_token;    /**< Auth token for auth middleware (optional). */
  } middleware;          /**< Middleware settings. */
} csilk_config_t;

/** @brief Load configuration from a YAML file.
 * @param yaml_path Path to the YAML file.
 * @param config Pointer to config struct to populate.
 * @return 0 on success, -1 on failure. */
int csilk_load_config(const char* yaml_path, csilk_config_t* config);

/** @brief Validate configuration values for semantic correctness.
 * @param config Pointer to config struct.
 * @param error_msg Optional pointer to store error string (static, do not
 * free).
 * @return 0 if valid, -1 if invalid with error_msg set. */
int csilk_config_validate(const csilk_config_t* config, const char** error_msg);

/** @brief Free all dynamically allocated strings in the configuration.
 * @param config Pointer to the config struct. */
void csilk_config_free(csilk_config_t* config);

/** @brief Auth validator callback. */
typedef int (*csilk_auth_validator_t)(const char* token);

/** @brief Simple token-based authentication middleware.
 * @param c The request context.
 * @param validator Callback function to validate the token. */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator);

/** @brief Serve static files from a directory.
 * @param c The request context.
 * @param root_dir Path to the local directory.
 * @note The URL prefix must be stripped before calling this function
 *       (the low-level API uses c->request.path directly). Use
 *       csilk_app_static() for automatic prefix handling. */
void csilk_static(csilk_ctx_t* c, const char* root_dir);

/** @brief Bind request body to a cJSON object.
 * @param c The request context.
 * @return Parsed cJSON pointer, or NULL if parsing fails. */
cJSON* csilk_bind_json(csilk_ctx_t* c);

/** @brief Bind request body to cJSON with error feedback.
 * @param c The request context.
 * @param error Pointer to store error string if parsing fails.
 * @return Parsed cJSON pointer, or NULL on error. */
cJSON* csilk_bind_json_err(csilk_ctx_t* c, const char** error);

/** @brief Get a cookie value by name.
 * @param c The request context.
 * @param name The cookie name.
 * @return The cookie value string, or NULL if not found. */
const char* csilk_get_cookie(csilk_ctx_t* c, const char* name);

/** @brief Set a cookie in the response.
 * @param c The request context.
 * @param name The cookie name.
 * @param value The cookie value.
 * @param max_age Maximum age in seconds (0 for session, -1 to delete).
 * @param path Cookie path (NULL for "/").
 * @param domain Cookie domain (NULL for current).
 * @param secure Flag for Secure attribute.
 * @param http_only Flag for HttpOnly attribute. */
void csilk_set_cookie(csilk_ctx_t* c, const char* name, const char* value,
                      int max_age, const char* path, const char* domain,
                      int secure, int http_only);

/** @brief Initialize the session system (call once on startup). */
void csilk_session_init(void);

/** @brief Start or resume a session for the current request.
 * Creates a new session cookie if none exists.
 * @param c The request context. */
void csilk_session_start(csilk_ctx_t* c);

/** @brief Store a value in the session.
 * @param c The request context.
 * @param key The key name.
 * @param value Pointer to the value to store. */
void csilk_session_set(csilk_ctx_t* c, const char* key, void* value);

/** @brief Retrieve a value from the session.
 * @param c The request context.
 * @param key The key name.
 * @return The stored value pointer, or NULL. */
void* csilk_session_get(csilk_ctx_t* c, const char* key);

/** @brief Destroy the current session and clear the cookie.
 * @param c The request context. */
void csilk_session_destroy(csilk_ctx_t* c);

/** @name Validation flags
 *  @{ */
#define CSILK_VALID_REQUIRED (1 << 0) /**< Field is required. */
#define CSILK_VALID_INT (1 << 1)      /**< Must be a valid integer. */
#define CSILK_VALID_STRING (1 << 2)   /**< Must be a string. */
#define CSILK_VALID_EMAIL (1 << 3)    /**< Must be a valid email format. */
/** @} */

/** @brief Single validation rule for request parameter validation. */
typedef struct csilk_valid_rule_s {
  const char* field; /**< Field name to validate. */
  int flags;         /**< Bitwise OR of CSILK_VALID_* flags. */
  int min;           /**< Minimum length (string) or value (int). */
  int max;           /**< Maximum length (string) or value (int). */
  const char*
      source; /**< "query", "form", "header", "cookie", or NULL for auto. */
} csilk_valid_rule_t;

/** @brief Validate request parameters against a set of rules.
 * Returns the field name of the first validation failure, or NULL on success.
 * @param c The request context.
 * @param rules NULL-terminated array of validation rules.
 * @return NULL on success, or pointer to the failing field name. */
const char* csilk_validate(csilk_ctx_t* c, const csilk_valid_rule_t* rules);

/** @brief Send a JSON response.
 * @param c The request context.
 * @param status HTTP status code.
 * @param json cJSON object (will be deleted by this function). */
void csilk_json(csilk_ctx_t* c, int status, cJSON* json);

/** @brief Send a JSON formatted error response.
 * @param c The request context.
 * @param status HTTP status code.
 * @param message Error description string. */
void csilk_json_error(csilk_ctx_t* c, int status, const char* message);

/** @brief Bind request body to a registered struct via reflection.
 * @param c The request context.
 * @param type_name Registered type name.
 * @param ptr Pointer to the struct to populate.
 * @return 1 on success, 0 on failure. */
int csilk_bind_reflect(csilk_ctx_t* c, const char* type_name, void* ptr);

/** @brief Send a JSON response from a registered struct via reflection.
 * @param c The request context.
 * @param status HTTP status code.
 * @param type_name Registered type name.
 * @param ptr Pointer to the struct. */
void csilk_json_reflect(csilk_ctx_t* c, int status, const char* type_name,
                        const void* ptr);

/** @brief Convenience macro for binding JSON body to a reflected struct.
 *  Wraps csilk_bind_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_bind(c, type, ptr) csilk_bind_reflect(c, #type, ptr)
/** @brief Convenience macro for sending a reflected struct as JSON response.
 *  Wraps csilk_json_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param status HTTP status code.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_json_t(c, status, type, ptr) \
  csilk_json_reflect(c, status, #type, ptr)

/** @brief Get the client's IP address.
 * @param c The request context.
 * @return The IP address string, or NULL on error. */
const char* csilk_get_client_ip(csilk_ctx_t* c);

/** @brief Internal: Split URL into path and query string.
 * @param url Full URL.
 * @param path Pointer to store the path string.
 * @param query Pointer to store the query string. */
void csilk_split_url(const char* url, char** path, char** query);

/** @brief Internal: Parse a raw query string into context.
 * @param c The request context.
 * @param query_string Raw query string (after '?'). */
void csilk_parse_query(csilk_ctx_t* c, const char* query_string);

/** @brief Router node structure. */
typedef struct csilk_router_node_s csilk_router_node_t;
/** @brief Main Router structure. */
typedef struct csilk_router_s {
  csilk_router_node_t* root; /**< Root node of the Radix Tree. */
} csilk_router_t;

/** @brief Route group structure. */
typedef struct csilk_group_s csilk_group_t;

/** @brief Create a new router instance.
 * @return Pointer to the new router. */
csilk_router_t* csilk_router_new();

/** @brief Add a route with handlers to the router.
 * @param r Router instance.
 * @param method HTTP method string.
 * @param path URL pattern (supports :param and *wildcard).
 * @param handlers Array of handler functions.
 * @param handler_count Number of handlers in the array. */
void csilk_router_add(csilk_router_t* r, const char* method, const char* path,
                      csilk_handler_t* handlers, size_t handler_count);

/** @brief Match a raw path to handlers (standalone).
 * @param r Router instance.
 * @param method HTTP method string.
 * @param path URL path string.
 * @return Pointer to handler array, or NULL if not found. */
csilk_handler_t* csilk_router_match(csilk_router_t* r, const char* method,
                                    const char* path);

/** @brief Match current request context to router.
 * Updates path parameters in context on success.
 * @param r Router instance.
 * @param c Request context.
 * @return 1 on success, 0 on failure. */
int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c);

/** @brief Deallocate router and all its nodes.
 * @param r Router instance. */
void csilk_router_free(csilk_router_t* r);

/** @brief Collect all registered routes from the router tree.
 * @param r Router instance.
 * @return A cJSON array of route objects (method, path, input_type,
 * output_type, summary, description). Caller must free with cJSON_Delete(). */
cJSON* csilk_router_collect_routes(csilk_router_t* r);

/** @brief Generate OpenAPI/Swagger 3.0 specification JSON from the router.
 *  Traverses all registered routes and uses the reflection system to generate
 *  JSON schemas for request/response types.
 * @param router The router instance.
 * @param title API title for the OpenAPI info section.
 * @param version API version for the OpenAPI info section.
 * @param description API description for the OpenAPI info section (optional).
 * @return A cJSON object representing the OpenAPI spec. Caller must free with
 * cJSON_Delete(). */
cJSON* csilk_generate_openapi_json(csilk_router_t* router, const char* title,
                                   const char* version,
                                   const char* description);

/** @brief Register a route with full metadata for OpenAPI spec generation.
 *  Extended version of csilk_router_add that also accepts metadata for
 *  automatic OpenAPI/Reflection documentation generation.
 * @param r Router instance.
 * @param method HTTP method string.
 * @param path URL pattern (supports :param and *wildcard).
 * @param handlers Array of handler functions.
 * @param handler_count Number of handlers in the array.
 * @param path_pattern The actual path pattern string (for metadata).
 * @param input_type Registered type name for request body binding (optional).
 * @param output_type Registered type name for response generation (optional).
 * @param summary Short summary of the operation (optional).
 * @param description Detailed description of the operation (optional). */
void csilk_router_add_extended(csilk_router_t* r, const char* method,
                               const char* path, csilk_handler_t* handlers,
                               size_t handler_count, const char* path_pattern,
                               const char* input_type, const char* output_type,
                               const char* summary, const char* description);

/** @brief Convenience macro to register a route with input/output type
 * metadata. */
#define CSILK_ROUTE(r, method, path, handlers, handler_count, input_type,   \
                    output_type, summary, desc)                             \
  csilk_router_add_extended(r, method, path, handlers, handler_count, path, \
                            input_type, output_type, summary, desc)

/** @brief Serve the OpenAPI JSON spec as a response in a handler.
 *  Call this from within a handler function to serve the spec.
 *
 *  @code
 *  void openapi_handler(csilk_ctx_t* c) {
 *      csilk_serve_openapi(c, router, "My API", "1.0.0", "API Description");
 *  }
 *  @endcode
 *
 *  @param c The request context.
 *  @param r The router instance.
 *  @param title API title.
 *  @param version API version.
 *  @param description API description (optional). */
void csilk_serve_openapi(csilk_ctx_t* c, csilk_router_t* r, const char* title,
                         const char* version, const char* description);

/** @brief Serve the embedded Swagger UI page that loads /openapi.json.
 *  Provides an interactive browser-based API documentation UI.
 *
 *  @code
 *  void docs_handler(csilk_ctx_t* c) {
 *      csilk_serve_swagger_ui(c);
 *  }
 *  @endcode
 *
 *  @param c The request context. */
void csilk_serve_swagger_ui(csilk_ctx_t* c);

/** @brief Create a new root group.
 * @param router Associated router.
 * @param prefix URL prefix for the group.
 * @return New route group instance. */
csilk_group_t* csilk_group_new(csilk_router_t* router, const char* prefix);

/** @brief Create a child group from an existing group.
 * @param parent Parent group.
 * @param prefix Sub-prefix.
 * @return New sub-group instance. */
csilk_group_t* csilk_group_group(csilk_group_t* parent, const char* prefix);

/** @brief Add middleware to the group.
 * @param group Route group.
 * @param handler Middleware function. */
void csilk_group_use(csilk_group_t* group, csilk_handler_t handler);

/** @brief Add a specific route to the group.
 * @param group Route group.
 * @param method HTTP method.
 * @param path URL pattern.
 * @param handler Route handler function. */
void csilk_group_add_route(csilk_group_t* group, const char* method,
                           const char* path, csilk_handler_t handler);

/** @brief Register a route handler with OpenAPI metadata.
 * @param group Route group.
 * @param method HTTP method.
 * @param path URL pattern.
 * @param handler Route handler function.
 * @param input_type Registered type name for request body (NULL if none).
 * @param output_type Registered type name for response (NULL if none).
 * @param summary Short operation summary (NULL if none).
 * @param description Detailed operation description (NULL if none). */
void csilk_group_add_route_extended(csilk_group_t* group, const char* method,
                                    const char* path, csilk_handler_t handler,
                                    const char* input_type,
                                    const char* output_type,
                                    const char* summary,
                                    const char* description);

/** @brief Add a route with multiple handlers (middleware + handler).
 * @param group Route group.
 * @param method HTTP method.
 * @param path URL pattern.
 * @param handlers Array of handler functions.
 * @param count Number of handlers in the array. */
void csilk_group_add_handlers(csilk_group_t* group, const char* method,
                              const char* path, csilk_handler_t* handlers,
                              size_t count);

/** @brief Deallocate a route group.
 * @param group Group instance. */
void csilk_group_free(csilk_group_t* group);

/** @name Group Route Macros
 * Convenience macros for adding routes to groups.
 * @{ */
/** @brief Register a GET route on the group. */
#define csilk_GET(group, path, handler) \
  csilk_group_add_route(group, "GET", path, handler)
/** @brief Register a POST route on the group. */
#define csilk_POST(group, path, handler) \
  csilk_group_add_route(group, "POST", path, handler)
/** @brief Register a PUT route on the group. */
#define csilk_PUT(group, path, handler) \
  csilk_group_add_route(group, "PUT", path, handler)
/** @brief Register a DELETE route on the group. */
#define csilk_DELETE(group, path, handler) \
  csilk_group_add_route(group, "DELETE", path, handler)
/** @brief Register a PATCH route on the group. */
#define csilk_PATCH(group, path, handler) \
  csilk_group_add_route(group, "PATCH", path, handler)
/** @brief Register an OPTIONS route on the group. */
#define csilk_OPTIONS(group, path, handler) \
  csilk_group_add_route(group, "OPTIONS", path, handler)
/** @brief Register a HEAD route on the group. */
#define csilk_HEAD(group, path, handler) \
  csilk_group_add_route(group, "HEAD", path, handler)
/** @} */

/** @brief Handshake and upgrade to WebSocket.
 * @param c The request context. */
void csilk_ws_handshake(csilk_ctx_t* c);

/** @brief Send a WebSocket message.
 * @param c The request context.
 * @param payload Data to send.
 * @param len Data length.
 * @param opcode Opcode (1 for text, 2 for binary). */
void csilk_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len,
                   int opcode);

/** @brief Send a WebSocket close frame with optional status code and reason.
 *  Initiates the close handshake per RFC 6455 Section 5.5.1.
 * @param c The request context.
 * @param status_code Status code (e.g., 1000 for normal, 0 to omit).
 * @param reason Optional close reason string (can be NULL). */
void csilk_ws_close(csilk_ctx_t* c, uint16_t status_code, const char* reason);

/* --- Streaming Response (Chunked Transfer Encoding) --- */

/** @brief Write data to the response stream using chunked transfer encoding.
 *  The first call sends HTTP response headers (with Transfer-Encoding:
 * chunked), then subsequent calls write chunked frames.  The handler MUST set
 *  is_async = 1 before calling this function and NOT call
 * csilk_string/json/etc.
 * @param c The request context.
 * @param data Data to write as the next chunk.
 * @param len Length of data. */
void csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len);

/** @brief Finalize a streaming response by sending the terminal chunk.
 *  After this call the connection is either kept alive or closed depending
 *  on the HTTP keep-alive header.
 * @param c The request context. */
void csilk_response_end(csilk_ctx_t* c);

/* --- Server-Sent Events (SSE) --- */

/** @brief Initialize an SSE connection.
 * Sends HTTP 200 response with text/event-stream headers.
 * Call this at the beginning of your SSE handler.
 * @param c The request context. */
void csilk_sse_init(csilk_ctx_t* c);

/** @brief Send an SSE event.
 * @param c The request context.
 * @param event Optional event type (NULL to omit).
 * @param data Event data string (NULL to send comment only). */
void csilk_sse_send(csilk_ctx_t* c, const char* event, const char* data);

/** @brief Close the SSE connection.
 * @param c The request context. */
void csilk_sse_close(csilk_ctx_t* c);

/* --- JWT Authentication Middleware --- */

/** @brief Generate a JWT token (HS256).
 * @param c Request context (for crypto driver access).
 * @param payload JSON object containing claims (e.g., {"sub": "123"}).
 * @param secret Secret key for signing.
 * @return Generated token string (must be freed by caller). */
char* csilk_jwt_generate(csilk_ctx_t* c, cJSON* payload, const char* secret);

/** @brief Verify and parse a JWT token.
 * @param c Request context (for crypto driver access).
 * @param token JWT token string.
 * @param secret Secret key for verification.
 * @return Parsed payload JSON object, or NULL if invalid. */
cJSON* csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret);

/** @brief JWT middleware handler.
 * Extracts Bearer token from Authorization header and verifies it.
 * If valid, stores payload in context storage ("jwt_payload") and continues.
 * If invalid, returns 401 Unauthorized.
 * @param c The request context.
 * @param secret Secret key for verification. */
void csilk_jwt_middleware(csilk_ctx_t* c, const char* secret);

/* --- Gzip Compression Middleware --- */

/** @brief Gzip response compression middleware.
 * Compresses response body if client accepts gzip encoding.
 * Must be used as a group-level middleware wrapping the handler.
 * @param c The request context. */
void csilk_gzip_middleware(csilk_ctx_t* c);

/* --- Multipart Form Data --- */

/** @brief A single part from a multipart/form-data request. */
typedef struct csilk_multipart_part_s {
  char name[128];        /**< Form field name. */
  char filename[256];    /**< Original filename (empty if not a file). */
  char content_type[64]; /**< Content-Type of the part. */
  uint8_t* data;         /**< Pointer to part body data. */
  size_t data_len;       /**< Length of part body data. */
  csilk_ctx_t* ctx;      /**< Owning request context. */
} csilk_multipart_part_t;

/** @brief Multipart handler callback.
 * Called for each part found in the multipart body.
 * @param part The parsed multipart part. */
typedef void (*csilk_multipart_handler_t)(csilk_multipart_part_t* part);

/** @brief Parse multipart/form-data request body.
 * Iterates through all parts and calls handler for each.
 * @param c The request context.
 * @param handler Callback for each part. */
void csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler);

/** @brief Main Server structure. */
typedef struct csilk_server_s csilk_server_t;

/* --- Hook System --- */

/** @brief Hook types for server and request lifecycle. */
typedef enum {
  CSILK_HOOK_SERVER_START,  /**< Called just before the server starts. */
  CSILK_HOOK_SERVER_STOP,   /**< Called when the server is stopping. */
  CSILK_HOOK_CONN_OPEN,     /**< Called when a new client connects. */
  CSILK_HOOK_CONN_CLOSE,    /**< Called when a client disconnects. */
  CSILK_HOOK_REQUEST_BEGIN, /**< Called when a request is fully parsed. */
  CSILK_HOOK_REQUEST_END,   /**< Called when a response is finished. */
  CSILK_HOOK_COUNT          /**< Total number of hook types. */
} csilk_hook_type_t;

/** @brief Callback for server-level hooks.
 * @param s The server instance. */
typedef void (*csilk_server_hook_handler_t)(csilk_server_t* s);

/** @brief Callback for context-level hooks.
 * @param c The request context. */
typedef void (*csilk_ctx_hook_handler_t)(csilk_ctx_t* c);

/** @brief Register a server-level hook.
 * @param s The server instance.
 * @param type The hook type.
 * @param handler The callback function. */
void csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type,
                           void* handler);

/* --- Crypto Driver Interface --- */

/** @brief Pluggable cryptographic algorithm driver.
 * Allows replacing default SHA256/HMAC/UUID implementations. */
typedef struct {
  /** @brief Compute SHA256 hash. */
  void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
  /** @brief Compute HMAC-SHA256. */
  void (*hmac_sha256)(const uint8_t* key, size_t key_len, const uint8_t* data,
                      size_t data_len, uint8_t out[32]);
  /** @brief Generate a random UUID v4 string. */
  void (*generate_uuid)(char buf[37]);
} csilk_crypto_driver_t;

/** @brief Set the global crypto driver for the server.
 * @param s The server instance.
 * @param driver Pointer to the crypto driver (NULL for default). */
void csilk_server_set_crypto_driver(csilk_server_t* s,
                                    csilk_crypto_driver_t* driver);

/** @brief Create a new server instance.
 * @param router The router to handle routing logic.
 * @return New server instance. */
csilk_server_t* csilk_server_new(csilk_router_t* router);

/** @brief Add global middleware to the server.
 * @param server Server instance.
 * @param handler Middleware function.
 * @return 0 on success, -1 if handler array is full. */
int csilk_server_use(csilk_server_t* server, csilk_handler_t handler);

/** @brief Set a custom handler for unmatched routes (404).
 * @param server Server instance.
 * @param handler Handler function (receives context with 404 already set).
 *                 Pass NULL to restore default behavior. */
void csilk_server_set_not_found_handler(csilk_server_t* server,
                                        csilk_handler_t handler);

/** @brief Enable SPA fallback: unmatched GET requests serve index.html
 * from doc_root. Overrides any custom 404 handler.
 * @param server Server instance.
 * @param doc_root Directory containing index.html (string is copied). */
void csilk_server_set_spa_fallback(csilk_server_t* server,
                                   const char* doc_root);

/** @brief Deallocate server resources.
 * @param server Server instance. */
void csilk_server_free(csilk_server_t* server);

/** @brief Signal the server to stop gracefully.
 * @param server Server instance. */
void csilk_server_stop(csilk_server_t* server);

/** @brief Apply server configuration.
 * @param server Server instance.
 * @param config Configuration struct (pointer, for efficiency). */
void csilk_server_set_config(csilk_server_t* server,
                             const csilk_server_config_t* config);

/** @brief Set the maximum number of concurrent client connections.
 * @param server Server instance.
 * @param max Maximum connections (0 = unlimited).
 * @return Previous limit value. */
int csilk_server_set_max_connections(csilk_server_t* server, int max);

/** @brief Set the storage driver for context key-value storage.
 * @param server Server instance.
 * @param driver Pointer to the storage driver (NULL for default in-memory). */
void csilk_server_set_storage_driver(csilk_server_t* server,
                                     csilk_storage_driver_t* driver);

/** @brief Run the server event loop.
 * Blocks until server is stopped or error occurs.
 * @param server Server instance.
 * @param port Port number to listen on.
 * @return 0 on success, -1 on failure. */
int csilk_server_run(csilk_server_t* server, int port);

/* --- Database Interface --- */

/** @brief Initialize the database system (registers built-in drivers). */
void csilk_db_init(void);

/** @brief Create a new database pool.
 * @param driver_name Driver name (e.g., "sqlite").
 * @param dsn Data source name.
 * @return New pool, or NULL on failure. */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/** @brief Free a database pool. */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/** @brief Execute a query and return JSON result.
 * @param pool Connection pool.
 * @param sql SQL statement.
 * @return cJSON array, or NULL on failure. Caller must free. */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/** @brief Execute a statement.
 * @param pool Connection pool.
 * @param sql SQL statement.
 * @return 0 on success, -1 on failure. */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/** @brief Query with parameters.
 * @param pool Connection pool.
 * @param sql SQL with ? placeholders.
 * @param params Parameter values (NULL-terminated).
 * @return cJSON array, or NULL on failure. */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql,
                                 const char** params);

/** @brief Prometheus metrics middleware.
 *  @param c The request context.
 *  @param arg Optional argument (unused). */
void csilk_metrics_middleware(csilk_ctx_t* c, const char* arg);

/** @brief Handler for /metrics endpoint.
 *  Returns metrics in Prometheus text format.
 *  @param c The request context. */
void csilk_metrics_handler(csilk_ctx_t* c);

#endif /* CSILK_H */
