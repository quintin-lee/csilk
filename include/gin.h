#ifndef GIN_H
#define GIN_H

#include <setjmp.h>
#include <stddef.h>

#include "cJSON.h"

/** @brief Gin framework version. */
#define GIN_VERSION "0.1.0"
/** @brief Maximum number of URL parameters. */
#define GIN_MAX_PARAMS 20

typedef struct gin_ctx_s gin_ctx_t;
typedef void (*gin_handler_t)(gin_ctx_t* c);

typedef struct gin_header_s gin_header_t;
struct gin_header_s {
  char* key;
  char* value;
  struct gin_header_s* next;
};

typedef struct {
  char* method;
  char* path;
  char* body;
  size_t body_len;
  gin_header_t* headers;
  gin_header_t* query_params;
} gin_request_t;

typedef struct {
  int status;
  const char* body;
  gin_header_t* headers;
  int body_is_managed;
} gin_response_t;

typedef struct {
  char* key;
  char* value;
} gin_param_t;

struct gin_ctx_s {
  int handler_index;
  gin_handler_t* handlers;  // NULL terminated array
  int aborted;
  jmp_buf jump_buffer;
  int has_jump_buffer;
  gin_request_t request;
  gin_response_t response;
  gin_param_t params[GIN_MAX_PARAMS];
  int params_count;
};

/** @brief Move to the next handler in the chain.
 * @param c The request context. */
void gin_next(gin_ctx_t* c);
/** @brief Abort the handler chain execution.
 * @param c The request context. */
void gin_abort(gin_ctx_t* c);
/** @brief Set the response status code.
 * @param c The request context.
 * @param status The HTTP status code. */
void gin_status(gin_ctx_t* c, int status);
/** @brief Set response body and status code.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param msg The response body message. */
void gin_string(gin_ctx_t* c, int status, const char* msg);
/** @brief Get a URL parameter by key.
 * @param c The request context.
 * @param key The parameter key.
 * @return The parameter value, or NULL if not found. */
const char* gin_get_param(gin_ctx_t* c, const char* key);
/** @brief Get a request header by key.
 * @param c The request context.
 * @param key The header key.
 * @return The header value, or NULL if not found. */
const char* gin_get_header(gin_ctx_t* c, const char* key);
/** @brief Get a query parameter by key.
 * @param c The request context.
 * @param key The query parameter key.
 * @return The query parameter value, or NULL if not found. */
const char* gin_get_query(gin_ctx_t* c, const char* key);
/** @brief Set a response header.
 * @param c The request context.
 * @param key The header key.
 * @param value The header value. */
void gin_set_header(gin_ctx_t* c, const char* key, const char* value);
/** @brief Clean up context resources.
 * @param c The request context. */
void gin_ctx_cleanup(gin_ctx_t* c);

/** @brief Recovery middleware handler.
 * @param c The request context. */
void gin_recovery_handler(gin_ctx_t* c);
/** @brief Panic in the handler.
 * @param c The request context. */
void gin_panic(gin_ctx_t* c);

/** @brief Logging middleware handler.
 * @param c The request context. */
void gin_logger_handler(gin_ctx_t* c);

typedef struct {
  const char* allow_origin;
  const char* allow_methods;
  const char* allow_headers;
  int allow_credentials;
  int max_age;
} gin_cors_config_t;
/** @brief CORS middleware.
 * @param c The request context.
 * @param config CORS configuration. */
void gin_cors_middleware(gin_ctx_t* c, gin_cors_config_t config);

typedef int (*gin_auth_validator_t)(const char* token);
/** @brief Authentication middleware.
 * @param c The request context.
 * @param validator The authentication validator function. */
void gin_auth_middleware(gin_ctx_t* c, gin_auth_validator_t validator);
/** @brief Static file serving middleware.
 * @param c The request context.
 * @param root_dir The root directory for static files. */
void gin_static(gin_ctx_t* c, const char* root_dir);

/** @brief Bind request body to JSON.
 * @param c The request context.
 * @return A cJSON pointer representing the parsed body, or NULL. */
cJSON* gin_bind_json(gin_ctx_t* c);
/** @brief Bind request body to JSON with error feedback.
 * @param c The request context.
 * @param error Optional pointer to store error message.
 * @return A cJSON pointer, or NULL on failure (check *error). */
cJSON* gin_bind_json_err(gin_ctx_t* c, const char** error);
/** @brief Set JSON response body and status.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param json The cJSON pointer to send as response. */
void gin_json(gin_ctx_t* c, int status, cJSON* json);
/** @brief Set a JSON error response with message.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param message The error message. */
void gin_json_error(gin_ctx_t* c, int status, const char* message);

/** @brief Internal helper to split URL path and query.
 * @param url The URL string.
 * @param path Pointer to store the path string.
 * @param query Pointer to store the query string. */
void gin_split_url(const char* url, char** path, char** query);
/** @brief Internal helper to parse query string.
 * @param c The request context.
 * @param query_string The query string to parse. */
void gin_parse_query(gin_ctx_t* c, const char* query_string);

typedef struct gin_router_node_s gin_router_node_t;
typedef struct gin_router_s {
  gin_router_node_t* root;
} gin_router_t;

typedef struct gin_group_s gin_group_t;

/** @brief Create a new router.
 * @return A new gin_router_t instance. */
gin_router_t* gin_router_new();
/** @brief Add a route to the router.
 * @param r The router.
 * @param method The HTTP method.
 * @param path The route path.
 * @param handlers Array of handlers.
 * @param handler_count Number of handlers. */
void gin_router_add(gin_router_t* r, const char* method, const char* path,
                    gin_handler_t* handlers, size_t handler_count);
/** @brief Match a route to handlers.
 * @param r The router.
 * @param method The HTTP method.
 * @param path The route path.
 * @return Array of handlers, or NULL if no match. */
gin_handler_t* gin_router_match(gin_router_t* r, const char* method,
                                const char* path);
/** @brief Match a route and update context.
 * @param r The router.
 * @param c The request context.
 * @return 1 if matched, 0 otherwise. */
int gin_router_match_ctx(gin_router_t* r, gin_ctx_t* c);
/** @brief Free the router.
 * @param r The router to free. */
void gin_router_free(gin_router_t* r);

/** @brief Create a new group.
 * @param router The router.
 * @param prefix The group prefix.
 * @return A new gin_group_t instance. */
gin_group_t* gin_group_new(gin_router_t* router, const char* prefix);
/** @brief Create a sub-group.
 * @param parent The parent group.
 * @param prefix The sub-group prefix.
 * @return A new gin_group_t instance. */
gin_group_t* gin_group_group(gin_group_t* parent, const char* prefix);
/** @brief Use middleware in the group.
 * @param group The group.
 * @param handler The middleware handler. */
void gin_group_use(gin_group_t* group, gin_handler_t handler);
/** @brief Add a route to the group.
 * @param group The group.
 * @param method The HTTP method.
 * @param path The route path.
 * @param handler The handler. */
void gin_group_add_route(gin_group_t* group, const char* method,
                         const char* path, gin_handler_t handler);
/** @brief Free the group.
 * @param group The group to free. */
void gin_group_free(gin_group_t* group);

// Convenience macros for groups
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

typedef struct gin_server_config_s {
  unsigned int idle_timeout_ms;    /**< Connection idle timeout (ms), default 5000 */
  size_t max_body_size;            /**< Max request body size (bytes), default 1048576 */
  int listen_backlog;              /**< TCP listen backlog, default 128 */
} gin_server_config_t;

typedef struct gin_server_s gin_server_t;
/** @brief Create a new server.
 * @param router The router to be used by the server.
 * @return A new gin_server_t instance. */
gin_server_t* gin_server_new(gin_router_t* router);
/** @brief Free the server.
 * @param server The server to free. */
void gin_server_free(gin_server_t* server);
/** @brief Stop the server gracefully.
 * @param server The server to stop. */
void gin_server_stop(gin_server_t* server);
/** @brief Configure server parameters.
 * @param server The server.
 * @param config The config struct. */
void gin_server_set_config(gin_server_t* server, gin_server_config_t config);
/** @brief Run the server.
 * @param server The server.
 * @param port The port to listen on.
 * @return 0 on success, -1 on failure. */
int gin_server_run(gin_server_t* server, int port);

#endif
