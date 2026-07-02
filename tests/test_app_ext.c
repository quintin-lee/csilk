#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/app.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int mid_called = 0;
static int hnd_called = 0;

static void
mid_handler(csilk_ctx_t* c)
{
    mid_called++;
    csilk_next(c);
}

static void
hello_handler(csilk_ctx_t* c)
{
    hnd_called++;
    csilk_string(c, CSILK_STATUS_OK, "hello");
}

int
main()
{
    printf("Testing csilk_app_new with nullptr config...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_log_level...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_log_level(app, CSILK_LOG_DEBUG);
        csilk_app_log_level(app, CSILK_LOG_INFO);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_log_json...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_log_json(app, 1);
        csilk_app_log_json(app, 0);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_log_file...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_log_file(app, "test_app_ext.log", 1024);
        csilk_app_log_file(app, nullptr, 0);
        csilk_app_free(app);
        remove("test_app_ext.log");
    }

    printf("Testing csilk_app_add_handlers...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_handler_t handlers[] = {hello_handler, nullptr};
        csilk_app_add_handlers(app, "GET", "/handle", handlers, 1);

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/handle");

        csilk_router_t* router = csilk_app_router(app);
        assert(router != nullptr);
        int matched = csilk_router_match_ctx(router, ctx);
        assert(matched);
        hnd_called = 0;
        csilk_next(ctx);
        assert(hnd_called == 1);

        csilk_test_ctx_free(ctx);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_add_route_extended...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_add_route_extended(
            app, "GET", "/extended", hello_handler, "Input", "Output", "Summary", "Description");

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/extended");

        csilk_router_t* router = csilk_app_router(app);
        assert(router != nullptr);
        int matched = csilk_router_match_ctx(router, ctx);
        assert(matched);
        hnd_called = 0;
        csilk_next(ctx);
        assert(hnd_called == 1);

        csilk_test_ctx_free(ctx);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_add_route_extended_perm...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_add_route_extended_perm(app,
                                          "DELETE",
                                          "/extperm/:id",
                                          hello_handler,
                                          nullptr,
                                          nullptr,
                                          "Del",
                                          "Delete",
                                          "admin",
                                          "users");

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "DELETE", "/extperm/42");

        csilk_router_t* router = csilk_app_router(app);
        assert(router != nullptr);
        int matched = csilk_router_match_ctx(router, ctx);
        assert(matched);
        hnd_called = 0;
        csilk_next(ctx);
        assert(hnd_called == 1);

        csilk_test_ctx_free(ctx);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_add_route_perm...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_add_route_perm(app, "GET", "/perm", hello_handler, "read", "documents");

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/perm");

        csilk_router_t* router = csilk_app_router(app);
        assert(router != nullptr);
        int matched = csilk_router_match_ctx(router, ctx);
        assert(matched);
        hnd_called = 0;
        csilk_next(ctx);
        assert(hnd_called == 1);

        csilk_test_ctx_free(ctx);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_use (server-level middleware)...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_use(app, mid_handler);
        csilk_app_add_route(app, "GET", "/midtest", hello_handler);
        csilk_server_t* srv = csilk_app_server(app);
        assert(srv != nullptr);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_use_group...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_use_group(app, "/api", mid_handler);
        csilk_app_add_route(app, "GET", "/api/test", hello_handler);

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        csilk_test_ctx_set_request(ctx, "GET", "/api/test");

        csilk_router_t* router = csilk_app_router(app);
        assert(router != nullptr);
        int matched = csilk_router_match_ctx(router, ctx);
        assert(matched);
        hnd_called = 0;
        csilk_next(ctx);
        assert(hnd_called == 1);

        csilk_test_ctx_free(ctx);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_apply_config...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_config_t* before = csilk_app_config(app);
        assert(before != nullptr);
        csilk_app_apply_config(app);
        free(before);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_enable_openapi...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_app_enable_openapi(app, 0);
        csilk_app_enable_openapi(app, 1);
        csilk_app_free(app);
    }

    printf("Testing csilk_app_set_server_config...\n");
    {
        csilk_app_t* app = csilk_app_new(nullptr);
        assert(app != nullptr);
        csilk_server_config_t scfg = {0};
        scfg.idle_timeout_ms = 9999;
        scfg.max_body_size = 65536;
        csilk_app_set_server_config(app, scfg);
        csilk_app_free(app);
    }

    printf("Testing csilk_app nullptr safety...\n");
    {
        csilk_app_add_route(nullptr, "GET", "/nope", hello_handler);
        csilk_app_add_handlers(nullptr, "GET", "/nope", nullptr, 0);
        csilk_app_add_route_perm(nullptr, "GET", "/nope", hello_handler, nullptr, nullptr);
        csilk_app_add_route_extended(
            nullptr, "GET", "/nope", hello_handler, nullptr, nullptr, nullptr, nullptr);
        csilk_app_add_route_extended_perm(nullptr,
                                          "GET",
                                          "/nope",
                                          hello_handler,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr);
        csilk_app_use(nullptr, mid_handler);
        csilk_app_use_group(nullptr, "/api", mid_handler);
        csilk_app_apply_config(nullptr);
        csilk_app_enable_openapi(nullptr, 0);
        csilk_app_log_level(nullptr, CSILK_LOG_INFO);
        csilk_app_log_json(nullptr, 0);
        csilk_app_log_file(nullptr, nullptr, 0);
        {
            csilk_server_config_t z = {0};
            csilk_app_set_server_config(nullptr, z);
        }
        csilk_config_t* cp = csilk_app_config(nullptr);
        assert(cp == nullptr);
    }

    printf("test_app_ext: PASS\n");
    return 0;
}
