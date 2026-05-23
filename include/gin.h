/**
 * @file gin.h
 * @brief High-performance C web framework inspired by Gin (Golang).
 * @version 0.1.0
 * @license MIT
 */

#ifndef GIN_H
#define GIN_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** @brief Gin framework version. */
#define GIN_VERSION "0.1.0"
/** @brief Maximum number of URL parameters. */
#define GIN_MAX_PARAMS 20

/** @forward_declaration Request context. */
typedef struct gin_ctx_s gin_ctx_t;
/** @brief Handler function pointer type. */
typedef void (*gin_handler_t)(gin_ctx_t* c);

/** @brief HTTP Header structure (linked list). */
typedef struct gin_header_s gin_header_t;
struct gin_header_s {
  char* key;                /**< Header field name. */
  char* value;              /**< Header field value. */
  struct gin_header_s* next; /**< Pointer to next header. */
};

/** @brief HTTP Request structure. */
typedef struct {
  char* method;             /**< HTTP method (e.g., "GET"). */
  char* path;               /**< Decoded URL path. */
  char* body;               /**< Raw request body. */
  size_t body_len;          /**< Length of the request body. */
  gin_header_t* headers;    /**< Linked list of request headers. */
  gin_header_t* query_params; /**< Parsed query parameters. */
} gin_request_t;

/** @brief HTTP Response structure. */
typedef struct {
  int status;               /**< HTTP status code. */
  const char* body;         /**< Response body content. */
  gin_header_t* headers;    /**< Linked list of response headers. */
  int body_is_managed;      /**< Flag if body is managed by free(). */
} gin_response_t;

/** @brief URL path parameter. */
typedef struct {
  char* key;                /**< Parameter key (from route, e.g., "id"). */
  char* value;              /**< Actual parameter value from URL. */
} gin_param_t;

/** @forward_declaration Arena allocator. */
typedef struct gin_arena_s gin_arena_t;

/** @brief Main Request Context.
 * Holds all information about the current HTTP request/response.
 */
struct gin_ctx_s {
  int handler_index;        /**< Index of current handler in the chain. */
  gin_handler_t* handlers;  /**< NULL terminated array of handlers. */
  int aborted;              /**< Flag if execution was aborted. */
  jmp_buf jump_buffer;      /**< Buffer for recovery (panic handling). */
  int has_jump_buffer;      /**< Flag if jump_buffer is active. */
  gin_arena_t* arena;       /**< Request-scoped arena allocator. */
  gin_request_t request;    /**< Request data. */
  gin_response_t response;  /**< Response data. */
  gin_param_t params[GIN_MAX_PARAMS]; /**< URL path parameters array. */
  int params_count;         /**< Current number of path parameters. */
  int is_websocket;         /**< Flag if connection is upgraded to WebSocket. */
  void (*on_ws_message)(gin_ctx_t* c, const uint8_t* payload, size_t len, int opcode);
  void* _internal_client;   /**< Internal client pointer (DO NOT USE). */
};

/** @brief Get the client's IP address.
 * @param c The request context.
 * @return The IP address string, or NULL on error. */
const char* gin_get_client_ip(gin_ctx_t* c);

/** @brief Handshake and upgrade to WebSocket.
 * @param c The request context. */
void gin_ws_handshake(gin_ctx_t* c);

/** @brief Send a WebSocket message.
 * @param c The request context.
 * @param payload Data to send.
 * @param len Data length.
 * @param opcode Opcode (1 for text, 2 for binary). */
void gin_ws_send(gin_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

/** @brief Move to the next handler in the onion model chain.
 * @param c The request context. */
void gin_next(gin_ctx_t* c);

/** @brief Abort the handler chain execution immediately.
 * @param c The request context. */
void gin_abort(gin_ctx_t* c);

/** @brief Set the response status code.
 * @param c The request context.
 * @param status The HTTP status code (e.g., 200, 404). */
void gin_status(gin_ctx_t* c, int status);

/** @brief Set response body and status code (Plain text/String).
 * Memory is managed by the request arena.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param msg The response body message string. */
void gin_string(gin_ctx_t* c, int status, const char* msg);

/** @brief Get a URL path parameter by key.
 * @param c The request context.
 * @param key The parameter key name.
 * @return The parameter value string, or NULL if not found. */
const char* gin_get_param(gin_ctx_t* c, const char* key);

/** @brief Get a request header by key.
 * Case-insensitive lookup.
 * @param c The request context.
 * @param key The header field name.
 * @return The header value string, or NULL if not found. */
const char* gin_get_header(gin_ctx_t* c, const char* key);

/** @brief Get a query parameter by key.
 * @param c The request context.
 * @param key The query parameter key name.
 * @return The query value string, or NULL if not found. */
const char* gin_get_query(gin_ctx_t* c, const char* key);

/** @brief Set a request header.
 * @param c The request context.
 * @param key The header field name.
 * @param value The header field value string. */
void gin_set_request_header(gin_ctx_t* c, const char* key, const char* value);

/** @brief Set a response header.
 * @param c The request context.
 * @param key The header field name.
 * @param value The header field value string. */
void gin_set_header(gin_ctx_t* c, const char* key, const char* value);

/** @brief Clean up all context-related resources (arena, headers, etc.).
 * @param c The request context. */
void gin_ctx_cleanup(gin_ctx_t* c);

/** @brief Create a new request-scoped arena allocator.
 * @param default_chunk_size Initial size of the arena memory chunk.
 * @return Pointer to the new arena instance. */
gin_arena_t* gin_arena_new(size_t default_chunk_size);

/** @brief Allocate memory from the arena.
 * Memory will be freed automatically in gin_ctx_cleanup.
 * @param arena The arena instance.
 * @param size Number of bytes to allocate. */
void* gin_arena_alloc(gin_arena_t* arena, size_t size);

/** @brief Duplicate a string using the arena allocator.
 * @param arena The arena instance.
 * @param s Source string.
 * @return Allocated copy of the string. */
char* gin_arena_strdup(gin_arena_t* arena, const char* s);

/** @brief Explicitly free the arena and all its chunks.
 * @param arena The arena instance. */
void gin_arena_free(gin_arena_t* arena);

/** @brief Recovery middleware handler.
 * Prevents server from crashing on panics and returns 500.
 * @param c The request context. */
void gin_recovery_handler(gin_ctx_t* c);

/** @brief Trigger a panic in the current handler.
 * Will be caught by the nearest recovery middleware.
 * @param c The request context. */
void gin_panic(gin_ctx_t* c);

/** @brief Logging middleware handler.
 * Logs request method, path, and processing time.
 * @param c The request context. */
void gin_logger_handler(gin_ctx_t* c);

/** @brief CORS middleware configuration. */
typedef struct {
  const char* allow_origin;      /**< Access-Control-Allow-Origin. */
  const char* allow_methods;     /**< Access-Control-Allow-Methods. */
  const char* allow_headers;     /**< Access-Control-Allow-Headers. */
  int allow_credentials;         /**< Access-Control-Allow-Credentials. */
  int max_age;                   /**< Access-Control-Max-Age. */
} gin_cors_config_t;

/** @brief Enable CORS with specified configuration.
 * @param c The request context.
 * @param config CORS settings. */
void gin_cors_middleware(gin_ctx_t* c, gin_cors_config_t config);

/** @brief Simple IP-based rate limiting middleware.
 * @param c The request context.
 * @param limit Maximum requests per minute. */
void gin_rate_limit_middleware(gin_ctx_t* c, int limit);

/** @brief Simple stateless CSRF protection middleware.
 * @param c The request context. */
void gin_csrf_middleware(gin_ctx_t* c);

/** @brief Auth validator callback. */
typedef int (*gin_auth_validator_t)(const char* token);

/** @brief Simple token-based authentication middleware.
 * @param c The request context.
 * @param validator Callback function to validate the token. */
void gin_auth_middleware(gin_ctx_t* c, gin_auth_validator_t validator);

/** @brief Serve static files from a directory.
 * @param c The request context.
 * @param root_dir Path to the local directory. */
void gin_static(gin_ctx_t* c, const char* root_dir);

/** @brief Bind request body to a cJSON object.
 * @param c The request context.
 * @return Parsed cJSON pointer, or NULL if parsing fails. */
cJSON* gin_bind_json(gin_ctx_t* c);

/** @brief Bind request body to cJSON with error feedback.
 * @param c The request context.
 * @param error Pointer to store error string if parsing fails.
 * @return Parsed cJSON pointer, or NULL on error. */
cJSON* gin_bind_json_err(gin_ctx_t* c, const char** error);

/** @brief Send a JSON response.
 * @param c The request context.
 * @param status HTTP status code.
 * @param json cJSON object (will be deleted by this function). */
void gin_json(gin_ctx_t* c, int status, cJSON* json);

/** @brief Send a JSON formatted error response.
 * @param c The request context.
 * @param status HTTP status code.
 * @param message Error description string. */
void gin_json_error(gin_ctx_t* c, int status, const char* message);

/** @brief Internal: Split URL into path and query string.
 * @param url Full URL.
 * @param path Pointer to store the path string.
 * @param query Pointer to store the query string. */
void gin_split_url(const char* url, char** path, char** query);

/** @brief Internal: Parse a raw query string into context.
 * @param c The request context.
 * @param query_string Raw query string (after '?'). */
void gin_parse_query(gin_ctx_t* c, const char* query_string);

/** @brief Router node structure. */
typedef struct gin_router_node_s gin_router_node_t;
/** @brief Main Router structure. */
typedef struct gin_router_s {
  gin_router_node_t* root;      /**< Root node of the Radix Tree. */
} gin_router_t;

/** @brief Route group structure. */
typedef struct gin_group_s gin_group_t;

/** @brief Create a new router instance.
 * @return Pointer to the new router. */
gin_router_t* gin_router_new();

/** @brief Add a route with handlers to the router.
 * @param r Router instance.
 * @param method HTTP method string.
 * @param path URL pattern (supports :param and *wildcard).
 * @param handlers Array of handler functions.
 * @param handler_count Number of handlers in the array. */
void gin_router_add(gin_router_t* r, const char* method, const char* path,
                    gin_handler_t* handlers, size_t handler_count);

/** @brief Match a raw path to handlers (standalone).
 * @param r Router instance.
 * @param method HTTP method string.
 * @param path URL path string.
 * @return Pointer to handler array, or NULL if not found. */
gin_handler_t* gin_router_match(gin_router_t* r, const char* method,
                                const char* path);

/** @brief Match current request context to router.
 * Updates path parameters in context on success.
 * @param r Router instance.
 * @param c Request context.
 * @return 1 on success, 0 on failure. */
int gin_router_match_ctx(gin_router_t* r, gin_ctx_t* c);

/** @brief Deallocate router and all its nodes.
 * @param r Router instance. */
void gin_router_free(gin_router_t* r);

/** @brief Create a new root group.
 * @param router Associated router.
 * @param prefix URL prefix for the group.
 * @return New route group instance. */
gin_group_t* gin_group_new(gin_router_t* router, const char* prefix);

/** @brief Create a child group from an existing group.
 * @param parent Parent group.
 * @param prefix Sub-prefix.
 * @return New sub-group instance. */
gin_group_t* gin_group_group(gin_group_t* parent, const char* prefix);

/** @brief Add middleware to the group.
 * @param group Route group.
 * @param handler Middleware function. */
void gin_group_use(gin_group_t* group, gin_handler_t handler);

/** @brief Add a specific route to the group.
 * @param group Route group.
 * @param method HTTP method.
 * @param path URL pattern.
 * @param handler Route handler function. */
void gin_group_add_route(gin_group_t* group, const char* method,
                         const char* path, gin_handler_t handler);

/** @brief Deallocate a route group.
 * @param group Group instance. */
void gin_group_free(gin_group_t* group);

/** @name Group Route Macros
 * Convenience macros for adding routes to groups.
 * @{ */
#define gin_GET(group, path, handler) \
  gin_group_add_route(group, "GET", path, handler)
#define gin_POST(group, path, handler) \
  gin_group_add_route(group, "POST", path, handler)
#define gin_PUT(group, path, handler) \
  gin_group_add_route(group, "PUT", path, handler)
#define gin_DELETE(group, path, handler) \
  gin_group_add_route(group, "DELETE", path, handler)
#define gin_PATCH(group, path, handler) \
  gin_group_add_route(group, "PATCH", path, handler)
#define gin_OPTIONS(group, path, handler) \
  gin_group_add_route(group, "OPTIONS", path, handler)
#define gin_HEAD(group, path, handler) \
  gin_group_add_route(group, "HEAD", path, handler)
/** @} */

/** @brief Server configuration options. */
typedef struct gin_server_config_s {
  unsigned int idle_timeout_ms;    /**< Connection idle timeout (ms). */
  size_t max_body_size;            /**< Maximum request body size. */
  int listen_backlog;              /**< TCP listen backlog. */
} gin_server_config_t;

/** @brief Main Server structure. */
typedef struct gin_server_s gin_server_t;

/** @brief Create a new server instance.
 * @param router The router to handle routing logic.
 * @return New server instance. */
gin_server_t* gin_server_new(gin_router_t* router);

/** @brief Deallocate server resources.
 * @param server Server instance. */
void gin_server_free(gin_server_t* server);

/** @brief Signal the server to stop gracefully.
 * @param server Server instance. */
void gin_server_stop(gin_server_t* server);

/** @brief Apply server configuration.
 * @param server Server instance.
 * @param config Configuration struct. */
void gin_server_set_config(gin_server_t* server, gin_server_config_t config);

/** @brief Run the server event loop.
 * Blocks until server is stopped or error occurs.
 * @param server Server instance.
 * @param port Port number to listen on.
 * @return 0 on success, -1 on failure. */
int gin_server_run(gin_server_t* server, int port);

#endif /* GIN_H */
