#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int middleware1_called = 0;
int middleware2_called = 0;
int handler_called = 0;

void
middleware1(csilk_ctx_t* c)
{
    middleware1_called++;
    csilk_next(c);
}

void
middleware2(csilk_ctx_t* c)
{
    middleware2_called++;
    csilk_next(c);
}

void
ping_handler(csilk_ctx_t* c)
{
    handler_called++;
    csilk_string(c, CSILK_STATUS_OK, "pong");
}

int
main()
{
    csilk_router_t* r = csilk_router_new();

    // Create root group
    csilk_group_t* api = csilk_group_new(r, "/api");
    csilk_group_use(api, middleware1);

    // Create nested group
    csilk_group_t* v1 = csilk_group_group(api, "/v1");
    csilk_group_use(v1, middleware2);

    // Add route to nested group
    csilk_GET(v1, "/ping", ping_handler);

    // Test match
    {
        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/api/v1/ping");

        int matched = csilk_router_match_ctx(r, ctx);
        assert(matched);

        // Execute handlers
        middleware1_called = 0;
        middleware2_called = 0;
        handler_called = 0;

        csilk_next(ctx);

        assert(middleware1_called == 1);
        assert(middleware2_called == 1);
        assert(handler_called == 1);
        assert(csilk_get_status(ctx) == CSILK_STATUS_OK);
        size_t body_len = 0;
        assert(strcmp(csilk_get_response_body(ctx, &body_len), "pong") == 0);

        csilk_test_ctx_free(ctx);
    }

    // Test path normalization (avoid double slashes)
    // /api/ + /v2/ + /hello -> /api/v2/hello
    csilk_group_t* api_slash = csilk_group_new(r, "/api/");
    csilk_group_t* v2_slash = csilk_group_group(api_slash, "/v2/");
    csilk_group_add_route(v2_slash, "GET", "/hello", ping_handler);

    {
        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/api/v2/hello");

        int matched = csilk_router_match_ctx(r, ctx);
        assert(matched);
        csilk_test_ctx_free(ctx);
    }

    // Cleanup
    csilk_group_free(v1);
    csilk_group_free(api);
    csilk_group_free(v2_slash);
    csilk_group_free(api_slash);
    csilk_router_free(r);

    printf("test_group: PASS\n");
    return 0;
}
