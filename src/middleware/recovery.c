#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "gin.h"

void gin_recovery_handler(gin_ctx_t *c) {
    if (setjmp(c->jump_buffer) == 0) {
        c->has_jump_buffer = 1;
        gin_next(c);
    } else {
        // Panic occurred, send 500
        gin_string(c, 500, "Internal Server Error");
    }
}

void gin_panic(gin_ctx_t *c) {
    if (c->has_jump_buffer) {
        longjmp(c->jump_buffer, 1);
    } else {
        fprintf(stderr, "Fatal: No recovery handler registered.\n");
        exit(1);
    }
}
