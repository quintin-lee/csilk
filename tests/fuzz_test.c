#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gin.h"

// Fuzz test for gin_split_url, gin_parse_query, and routing

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;

    // Create null-terminated string
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    // 1. Fuzz gin_split_url
    char *path = NULL;
    char *query = NULL;
    gin_split_url(input, &path, &query);

    gin_ctx_t ctx = {0};

    // 2. Fuzz gin_parse_query
    if (query) {
        gin_parse_query(&ctx, query);
    }

    // 3. Fuzz routing
    gin_router_t *router = gin_router_new();
    if (router) {
        gin_handler_t handlers[] = {NULL};
        gin_router_add(router, "GET", "/api/:id/users/*action", handlers, 1);
        gin_router_add(router, "POST", "/api/ping", handlers, 1);

        if (path) {
            ctx.request.method = "GET";
            ctx.request.path = path;
            gin_router_match_ctx(router, &ctx);
        }
        gin_router_free(router);
    }

    if (path) free(path);
    if (query) free(query);
    gin_ctx_cleanup(&ctx);
    free(input);

    return 0;
}
