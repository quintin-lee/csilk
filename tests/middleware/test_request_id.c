#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_request_id_null_context()
{
    printf("Testing request_id middleware with nullptr context...\n");
    csilk_request_id_middleware(nullptr);
    printf("request_id nullptr context passed!\n");
}

static void
test_request_id_generates_id()
{
    printf("Testing request_id middleware generates UUID...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_request_id_middleware(ctx);

    const char* rid = csilk_get_request_id(ctx);
    assert(rid != nullptr && rid[0] != '\0');
    assert(strlen(rid) == 36);

    const char* hdr = csilk_get_response_header(ctx, "X-Request-Id");
    assert(hdr != nullptr);
    assert(strcmp(hdr, rid) == 0);

    csilk_test_ctx_free(ctx);
    printf("request_id generates UUID passed!\n");
}

static void
test_request_id_preserves_existing()
{
    printf("Testing request_id middleware preserves existing ID...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_set_request_id(ctx, "abc-123-def");

    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_request_id_middleware(ctx);

    assert(strcmp(csilk_get_request_id(ctx), "abc-123-def") == 0);

    const char* hdr = csilk_get_response_header(ctx, "X-Request-Id");
    assert(hdr != nullptr);
    assert(strcmp(hdr, "abc-123-def") == 0);

    csilk_test_ctx_free(ctx);
    printf("request_id preserves existing ID passed!\n");
}

static void
test_health_check_handler_null()
{
    printf("Testing health check handler with nullptr...\n");
    csilk_health_check_handler(nullptr);
    printf("health check nullptr passed!\n");
}

static void
test_health_check_handler()
{
    printf("Testing health check handler...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_health_check_handler(ctx);

    assert(csilk_get_status(ctx) == CSILK_STATUS_OK);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr);
    assert(strstr(body, "status") != nullptr);
    assert(strstr(body, "up") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("health check handler passed!\n");
}

int
main()
{
    test_request_id_null_context();
    test_request_id_generates_id();
    test_request_id_preserves_existing();
    test_health_check_handler_null();
    test_health_check_handler();
    printf("test_request_id: ALL PASSED\n");
    return 0;
}
