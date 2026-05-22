#include "gin.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

void mock_handler1(gin_ctx_t *c) { (void)c; }
void mock_handler2(gin_ctx_t *c) { (void)c; }

int main() {
    gin_router_t *r = gin_router_new();
    assert(r != NULL);

    gin_router_add(r, "GET", "/hello", mock_handler1);
    gin_router_add(r, "POST", "/submit", mock_handler2);

    assert(gin_router_match(r, "GET", "/hello") == mock_handler1);
    assert(gin_router_match(r, "POST", "/submit") == mock_handler2);
    assert(gin_router_match(r, "GET", "/notfound") == NULL);
    assert(gin_router_match(r, "POST", "/hello") == NULL);

    gin_router_free(r);
    printf("test_router passed!\n");
    return 0;
}
