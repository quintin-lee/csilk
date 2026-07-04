#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int handler_called = 0;

static void
test_handler(csilk_ctx_t* c)
{
    (void)c;
    handler_called++;
}

static void
test_ratelimit_basic()
{
    printf("Testing rate limit basic...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    handler_called = 0;
    csilk_rate_limit_middleware(ctx, 100);
    assert(handler_called == 1);
    assert(csilk_is_aborted(ctx) == 0);

    csilk_test_ctx_free(ctx);
    printf("Rate limit basic test passed!\n");
}

int
main()
{
    test_ratelimit_basic();
    printf("test_ratelimit: ALL PASSED\n");
    return 0;
}
