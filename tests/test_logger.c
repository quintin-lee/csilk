#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../include/gin.h"

// Mock handler
void dummy_handler(gin_ctx_t *c) {
    c->response.status = 200;
}

int main() {
    // Setup mock context
    gin_ctx_t c;
    memset(&c, 0, sizeof(gin_ctx_t));
    c.request.method = "GET";
    c.request.path = "/test";
    
    // Setup handlers
    gin_handler_t handlers[] = {gin_logger_handler, dummy_handler, NULL};
    c.handlers = handlers;
    c.handler_index = 0;

    printf("Running test_logger...\n");
    gin_logger_handler(&c);

    assert(c.response.status == 200);
    printf("test_logger passed!\n");

    return 0;
}
