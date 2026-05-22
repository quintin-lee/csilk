#ifndef GIN_H
#define GIN_H

#include <stddef.h>

#define GIN_VERSION "0.1.0"
#define GIN_MAX_PARAMS 20

typedef struct gin_ctx_s gin_ctx_t;
typedef void (*gin_handler_t)(gin_ctx_t *c);

typedef struct gin_header_s gin_header_t;
struct gin_header_s {
    char *key;
    char *value;
    struct gin_header_s *next;
};

typedef struct {
    char *method;
    char *path;
    char *body;
    gin_header_t *headers;
    gin_header_t *query_params;
} gin_request_t;

typedef struct {
    int status;
    const char *body;
    gin_header_t *headers;
} gin_response_t;

typedef struct {
    char *key;
    char *value;
} gin_param_t;

struct gin_ctx_s {
    int handler_index;
    gin_handler_t *handlers; // NULL terminated array
    int aborted;
    gin_request_t request;
    gin_response_t response;
    gin_param_t params[GIN_MAX_PARAMS];
    int params_count;
};

void gin_next(gin_ctx_t *c);
void gin_abort(gin_ctx_t *c);
void gin_status(gin_ctx_t *c, int status);
void gin_string(gin_ctx_t *c, int status, const char *msg);
const char* gin_get_param(gin_ctx_t *c, const char *key);
const char* gin_get_header(gin_ctx_t *c, const char *key);
const char* gin_get_query(gin_ctx_t *c, const char *key);
void gin_set_header(gin_ctx_t *c, const char *key, const char *value);
void gin_ctx_cleanup(gin_ctx_t *c);

// Internal URL helpers
void gin_split_url(const char *url, char **path, char **query);
void gin_parse_query(gin_ctx_t *c, const char *query_string);

typedef struct gin_router_node_s gin_router_node_t;
typedef struct gin_router_s {
    gin_router_node_t *root;
} gin_router_t;

typedef struct gin_group_s gin_group_t;

gin_router_t* gin_router_new();
void gin_router_add(gin_router_t *r, const char *method, const char *path, gin_handler_t *handlers, size_t handler_count);
gin_handler_t* gin_router_match(gin_router_t *r, const char *method, const char *path);
int gin_router_match_ctx(gin_router_t *r, gin_ctx_t *c);
void gin_router_free(gin_router_t *r);

gin_group_t* gin_group_new(gin_router_t *router, const char *prefix);
gin_group_t* gin_group_group(gin_group_t *parent, const char *prefix);
void gin_group_use(gin_group_t *group, gin_handler_t handler);
void gin_group_add_route(gin_group_t *group, const char *method, const char *path, gin_handler_t handler);
void gin_group_free(gin_group_t *group);

// Convenience macros for groups
#define gin_GET(group, path, handler) gin_group_add_route(group, "GET", path, handler)
#define gin_POST(group, path, handler) gin_group_add_route(group, "POST", path, handler)
#define gin_PUT(group, path, handler) gin_group_add_route(group, "PUT", path, handler)
#define gin_DELETE(group, path, handler) gin_group_add_route(group, "DELETE", path, handler)
#define gin_PATCH(group, path, handler) gin_group_add_route(group, "PATCH", path, handler)
#define gin_OPTIONS(group, path, handler) gin_group_add_route(group, "OPTIONS", path, handler)
#define gin_HEAD(group, path, handler) gin_group_add_route(group, "HEAD", path, handler)

typedef struct gin_server_s gin_server_t;
gin_server_t* gin_server_new(gin_router_t *router);
void gin_server_free(gin_server_t *server);
int gin_server_run(gin_server_t *server, int port);

#endif
