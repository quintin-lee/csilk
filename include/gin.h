#ifndef GIN_H
#define GIN_H

#define GIN_VERSION "0.1.0"

typedef struct gin_ctx_s gin_ctx_t;
typedef void (*gin_handler_t)(gin_ctx_t *c);

struct gin_ctx_s {
    int handler_index;
    gin_handler_t *handlers; // NULL terminated array
    int aborted;
};

void gin_next(gin_ctx_t *c);
void gin_abort(gin_ctx_t *c);

typedef struct gin_router_node_s gin_router_node_t;
typedef struct gin_router_s {
    gin_router_node_t *root;
} gin_router_t;

gin_router_t* gin_router_new();
void gin_router_add(gin_router_t *r, const char *method, const char *path, gin_handler_t handler);
gin_handler_t gin_router_match(gin_router_t *r, const char *method, const char *path);
void gin_router_free(gin_router_t *r);

#endif
