#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_sliding_ratelimit_basic()
{
    printf("Testing Sliding Window Rate Limiter basic...\n");

    csilk_sliding_limiter_t* lim = csilk_sliding_limiter_new(3, 1000);
    assert(lim != NULL);

    for (int i = 0; i < 3; i++) {
        csilk_ctx_t*    ctx = csilk_test_ctx_new();
        csilk_handler_t handlers[] = {nullptr};
        csilk_test_ctx_set_handlers(ctx, handlers);

        csilk_sliding_rate_limit_middleware(ctx, lim);
        assert(csilk_get_status(ctx) != 429);
        csilk_test_ctx_free(ctx);
    }

    // 4th request should exceed limit and get 429
    csilk_ctx_t*    ctx_exceeded = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx_exceeded, handlers);

    csilk_sliding_rate_limit_middleware(ctx_exceeded, lim);
    assert(csilk_get_status(ctx_exceeded) == 429);

    const char* retry_hdr = csilk_get_response_header(ctx_exceeded, "Retry-After");
    assert(retry_hdr != NULL);

    csilk_test_ctx_free(ctx_exceeded);
    csilk_sliding_limiter_free(lim);

    printf("test_sliding_ratelimit_basic: PASS\n");
}

int
main()
{
    test_sliding_ratelimit_basic();
    printf("All Sliding Window Rate Limiter tests passed successfully!\n");
    return 0;
}
