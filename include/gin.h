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

/** @brief Move to the next handler in the chain. */
void gin_next(gin_ctx_t* c);
/** @brief Abort the handler chain execution. */
void gin_abort(gin_ctx_t* c);
/** @brief Set the response status code. */
void gin_status(gin_ctx_t* c, int status);
/** @brief Set response body and status code. */
void gin_string(gin_ctx_t* c, int status, const char* msg);
/** @brief Get a URL parameter by key. */
const char* gin_get_param(gin_ctx_t* c, const char* key);
/** @brief Get a request header by key. */
const char* gin_get_header(gin_ctx_t* c, const char* key);
/** @brief Get a query parameter by key. */
const char* gin_get_query(gin_ctx_t* c, const char* key);
/** @brief Set a response header. */
void gin_set_header(gin_ctx_t* c, const char* key, const char* value);
/** @brief Clean up context resources. */
void gin_ctx_cleanup(gin_ctx_t* c);

/** @brief Recovery middleware handler. */
void gin_recovery_handler(gin_ctx_t* c);
/** @brief Panic in the handler. */
void gin_panic(gin_ctx_t* c);

/** @brief Logging middleware handler. */
void gin_logger_handler(gin_ctx_t* c);

typedef int (*gin_auth_validator_t)(const char* token);
/** @brief Authentication middleware. */
void gin_auth_middleware(gin_ctx_t* c, gin_auth_validator_t validator);
/** @brief Static file serving middleware. */
void gin_static(gin_ctx_t* c, const char* root_dir);

/** @brief Bind request body to JSON. */
cJSON* gin_bind_json(gin_ctx_t* c);
/** @brief Set JSON response body and status. */
void gin_json(gin_ctx_t* c, int status, cJSON* json);

/** @brief Internal helper to split URL path and query. */
void gin_split_url(const char* url, char** path, char** query);
/** @brief Internal helper to parse query string. */
void gin_parse_query(gin_ctx_t* c, const char* query_string);

typedef struct gin_router_node_s gin_router_node_t;
typedef struct gin_router_s {
  gin_router_node_t* root;
} gin_router_t;

typedef struct gin_group_s gin_group_t;

/** @brief Create a new router. */
gin_router_t* gin_router_new();
/** @brief Add a route to the router. */
void gin_router_add(gin_router_t* r, const char* method, const char* path,
                    gin_handler_t* handlers, size_t handler_count);
/** @brief Match a route to handlers. */
gin_handler_t* gin_router_match(gin_router_t* r, const char* method,
                                const char* path);
/** @brief Match a route and update context. */
int gin_router_match_ctx(gin_router_t* r, gin_ctx_t* c);
/** @brief Free the router. */
void gin_router_free(gin_router_t* r);

/** @brief Create a new group. */
gin_group_t* gin_group_new(gin_router_t* router, const char* prefix);
/** @brief Create a sub-group. */
gin_group_t* gin_group_group(gin_group_t* parent, const char* prefix);
/** @brief Use middleware in the group. */
void gin_group_use(gin_group_t* group, gin_handler_t handler);
/** @brief Add a route to the group. */
void gin_group_add_route(gin_group_t* group, const char* method,
                         const char* path, gin_handler_t handler);
/** @brief Free the group. */
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

typedef struct gin_server_s gin_server_t;
/** @brief Create a new server. */
gin_server_t* gin_server_new(gin_router_t* router);
/** @brief Free the server. */
void gin_server_free(gin_server_t* server);
/** @brief Run the server. */
int gin_server_run(gin_server_t* server, int port);

#endif
