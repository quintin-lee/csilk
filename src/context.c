#include "gin.h"
#include <stddef.h>

void gin_next(gin_ctx_t *c) {
    if (c->aborted) return;
    c->handler_index++;
    if (c->handlers[c->handler_index] != NULL) {
        c->handlers[c->handler_index](c);
    }
}

void gin_abort(gin_ctx_t *c) {
    c->aborted = 1;
}
