#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include "gin.h"

int counter = 0;

void m1(gin_ctx_t *c) {
    counter++;
    gin_next(c);
    counter++;
}

void m2(gin_ctx_t *c) {
    counter++;
    gin_next(c);
    counter++;
}

void handler(gin_ctx_t *c) {
    counter++;
}

void test_basic_chaining() {
    counter = 0;
    gin_handler_t handlers[] = {m1, m2, handler, NULL};
    gin_ctx_t c = {
        .handler_index = -1,
        .handlers = handlers,
        .aborted = 0
    };
    gin_next(&c);
    assert(counter == 5); // m1++, m2++, handler++, m2++, m1++
    printf("test_basic_chaining passed\n");
}

void m_abort(gin_ctx_t *c) {
    counter++;
    gin_abort(c);
}

void test_abort() {
    counter = 0;
    gin_handler_t handlers[] = {m_abort, handler, NULL};
    gin_ctx_t c = {
        .handler_index = -1,
        .handlers = handlers,
        .aborted = 0
    };
    gin_next(&c);
    assert(counter == 1);
    printf("test_abort passed\n");
}

int main() {
    test_basic_chaining();
    test_abort();
    return 0;
}
