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

#endif
