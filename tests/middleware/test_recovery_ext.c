#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static volatile int panic_caught = 0;

static void
handler_that_panics(csilk_ctx_t* c)
{
    csilk_panic(c);
}

static void
test_recovery_handler_catches_panic()
{
    printf("Testing recovery handler catches panic...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_handler_t handlers[] = {handler_that_panics, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_recovery_handler(ctx);

    assert(csilk_get_status(ctx) == CSILK_STATUS_INTERNAL_SERVER_ERROR);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr);
    assert(strstr(body, "Internal Server Error") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("Recovery handler caught panic passed!\n");
}

static int normal_handler_called = 0;

static void
normal_handler(csilk_ctx_t* c)
{
    normal_handler_called = 1;
}

static void
test_recovery_handler_normal_flow()
{
    printf("Testing recovery handler normal flow...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    normal_handler_called = 0;
    csilk_handler_t handlers[] = {normal_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_recovery_handler(ctx);

    assert(normal_handler_called == 1);

    csilk_test_ctx_free(ctx);
    printf("Recovery handler normal flow passed!\n");
}

int
main()
{
    test_recovery_handler_catches_panic();
    test_recovery_handler_normal_flow();
    printf("test_recovery_ext: ALL PASSED\n");
    return 0;
}
