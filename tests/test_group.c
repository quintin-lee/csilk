#include "gin.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int middleware1_called = 0;
int middleware2_called = 0;
int handler_called = 0;

void middleware1(gin_ctx_t *c) {
    middleware1_called++;
    gin_next(c);
}

void middleware2(gin_ctx_t *c) {
    middleware2_called++;
    gin_next(c);
}

void ping_handler(gin_ctx_t *c) {
    handler_called++;
    gin_string(c, 200, "pong");
}

int main() {
    gin_router_t *r = gin_router_new();
    
    // Create root group
    gin_group_t *api = gin_group_new(r, "/api");
    gin_group_use(api, middleware1);
    
    // Create nested group
    gin_group_t *v1 = gin_group_group(api, "/v1");
    gin_group_use(v1, middleware2);
    
    // Add route to nested group
    gin_GET(v1, "/ping", ping_handler);
    
    // Test match
    {
        gin_ctx_t ctx = {0};
        ctx.request.method = "GET";
        ctx.request.path = "/api/v1/ping";
        
        int matched = gin_router_match_ctx(r, &ctx);
        assert(matched);
        assert(ctx.handlers != NULL);
        
        // Execute handlers
        middleware1_called = 0;
        middleware2_called = 0;
        handler_called = 0;
        
        ctx.handler_index = -1;
        gin_next(&ctx);
        
        assert(middleware1_called == 1);
        assert(middleware2_called == 1);
        assert(handler_called == 1);
        assert(ctx.response.status == 200);
        assert(strcmp(ctx.response.body, "pong") == 0);
        
        gin_ctx_cleanup(&ctx);
    }
    
    // Test path normalization (avoid double slashes)
    // /api/ + /v2/ + /hello -> /api/v2/hello
    gin_group_t *api_slash = gin_group_new(r, "/api/");
    gin_group_t *v2_slash = gin_group_group(api_slash, "/v2/");
    gin_group_add_route(v2_slash, "GET", "/hello", ping_handler);
    
    {
        gin_ctx_t ctx = {0};
        ctx.request.method = "GET";
        ctx.request.path = "/api/v2/hello";
        
        int matched = gin_router_match_ctx(r, &ctx);
        assert(matched);
        gin_ctx_cleanup(&ctx);
    }

    // Cleanup
    gin_group_free(v1);
    gin_group_free(api);
    gin_group_free(v2_slash);
    gin_group_free(api_slash);
    gin_router_free(r);
    
    printf("test_group: PASS\n");
    return 0;
}
