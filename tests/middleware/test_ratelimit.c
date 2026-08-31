#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

static void
test_ratelimit_fail_open_on_saturated_table()
{
    printf("Testing rate limit fail-open on saturated table...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    /* Fill the entire 65536-slot IP table with distinct IPs, each limited to 1
     * request per window. A fresh key always lands in its own slot (count ==
     * limit is allowed), so none of these fills is blocked, and inserting
     * exactly 65536 distinct keys necessarily leaves the open-addressed table
     * full. Reusing one ctx is fine: the table lives in ratelimit.c and is
     * independent of the handler chain. */
    for (int i = 0; i < 65536; i++) {
        char ip[32];
        snprintf(ip, sizeof(ip), "10.0.0.%d", i);
        _csilk_rate_limit_local(ctx, ip, 1);
        assert(csilk_is_aborted(ctx) == 0); /* fresh IP allowed, never blocked */
    }
    csilk_test_ctx_free(ctx);

    /* Table is now saturated. A brand-new IP must fail open (be allowed), not
     * fall back to sharing a hash slot: that shared slot's count is already at
     * the limit, so an innocent client would be wrongly blocked and confused
     * with whichever IP owns the slot. Use a fresh ctx so the handler chain
     * (and hence csilk_next) starts from index 0. */
    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t handlers2[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx2, handlers2);

    handler_called = 0;
    _csilk_rate_limit_local(ctx2, "203.0.113.1", 1);
    assert(handler_called == 1); /* chain still invoked: fail-open, not blocked */
    assert(csilk_is_aborted(ctx2) == 0);

    csilk_test_ctx_free(ctx2);
    printf("Rate limit fail-open test passed!\n");
}

static void
test_ratelimit_retry_after_precise()
{
    printf("Testing rate limit precise Retry-After...\n");
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    /* First request from a fresh IP with limit=1 is allowed (fresh handler chain). */
    handler_called = 0;
    _csilk_rate_limit_local(ctx, "10.10.10.10", 1);
    assert(handler_called == 1);
    assert(csilk_is_aborted(ctx) == 0);
    csilk_test_ctx_free(ctx);

    /* Second request to the same IP within the window is blocked with a 429.
     * Use a fresh ctx so the handler chain restarts at index 0 — the shared IP
     * count lives in the global table, independent of the context. */
    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t handlers2[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx2, handlers2);
    handler_called = 0;
    _csilk_rate_limit_local(ctx2, "10.10.10.10", 1);
    assert(handler_called == 0);
    assert(csilk_is_aborted(ctx2) == 1);

    /* Retry-After must be a positive integer <= WINDOW_SIZE (60), reflecting
     * the actual time remaining in the current window, not a hardcoded 60. */
    const char* retry_hdr = csilk_get_response_header(ctx2, "Retry-After");
    assert(retry_hdr != NULL);
    int retry_after = atoi(retry_hdr);
    assert(retry_after >= 1 && retry_after <= 60);

    csilk_test_ctx_free(ctx2);
    printf("Rate limit precise Retry-After test passed!\n");
}

int
main()
{
    /* Order matters: the fail-open test fills (and never empties) the shared
     * 65536-slot IP table, so any counting test that needs real slots must run
     * before it. Run the precise Retry-After (counting) test first. */
    test_ratelimit_basic();
    test_ratelimit_retry_after_precise();
    test_ratelimit_fail_open_on_saturated_table();
    printf("test_ratelimit: ALL PASSED\n");
    return 0;
}
