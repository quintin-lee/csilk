#include "gin.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

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

void gin_status(gin_ctx_t *c, int status) {
    c->response.status = status;
}

void gin_string(gin_ctx_t *c, int status, const char *msg) {
    c->response.status = status;
    c->response.body = msg;
}

const char* gin_get_param(gin_ctx_t *c, const char *key) {
    for (int i = 0; i < c->params_count; i++) {
        if (strcmp(c->params[i].key, key) == 0) {
            return c->params[i].value;
        }
    }
    return NULL;
}

void gin_ctx_cleanup(gin_ctx_t *c) {
    if (!c) return;
    for (int i = 0; i < c->params_count; i++) {
        free(c->params[i].key);
        free(c->params[i].value);
    }
    c->params_count = 0;
}
