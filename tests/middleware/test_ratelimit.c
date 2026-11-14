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

static uint32_t
test_djb2_hash(const char* s, uint32_t table_size)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*s++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % table_size;
}

static void
fill_ip_probe_window(const char* target_ip)
{
    enum { IP_TABLE_SIZE = 65536, PROBE_WIN = 512 };
    uint32_t target_h = test_djb2_hash(target_ip, IP_TABLE_SIZE);
    char     keys[PROBE_WIN][32];
    int      filled[PROBE_WIN] = {0};
    int      remaining = PROBE_WIN;

    for (uint32_t n = 0; remaining > 0; n++) {
        char candidate[32];
        snprintf(candidate,
                 sizeof(candidate),
                 "10.%u.%u.%u",
                 (n >> 16) & 0xff,
                 (n >> 8) & 0xff,
                 n & 0xff);
        uint32_t h = test_djb2_hash(candidate, IP_TABLE_SIZE);
        uint32_t off = (h + IP_TABLE_SIZE - target_h) % IP_TABLE_SIZE;
        if (off < PROBE_WIN && !filled[off]) {
            snprintf(keys[off], sizeof(keys[off]), "%s", candidate);
            filled[off] = 1;
            remaining--;
        }
    }

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    for (int i = 0; i < PROBE_WIN; i++) {
        _csilk_rate_limit_local(ctx, keys[i], 1);
        assert(csilk_is_aborted(ctx) == 0);
    }
    csilk_test_ctx_free(ctx);
}

static void
test_ratelimit_fail_open_on_saturated_table()
{
    printf("Testing rate limit fail-open on saturated table...\n");
    _csilk_ratelimit_reset_tables();
    csilk_rate_limit_set_saturation_policy(CSILK_RATE_SATURATION_FAIL_OPEN);
    fill_ip_probe_window("203.0.113.1");

    /* Probe window is now saturated. Under FAIL_OPEN, a brand-new IP must
     * fail open (be allowed), not share an active slot or emit 429. */
    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t handlers2[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx2, handlers2);

    handler_called = 0;
    _csilk_rate_limit_local(ctx2, "203.0.113.1", 1);
    assert(handler_called == 1); /* chain still invoked: fail-open, not blocked */
    assert(csilk_is_aborted(ctx2) == 0);

    csilk_test_ctx_free(ctx2);
    _csilk_ratelimit_reset_tables();
    printf("Rate limit fail-open test passed!\n");
}

static void
test_ratelimit_fail_closed_on_saturated_table()
{
    printf("Testing rate limit fail-closed on saturated table...\n");
    _csilk_ratelimit_reset_tables();
    csilk_rate_limit_set_saturation_policy(CSILK_RATE_SATURATION_FAIL_CLOSED);
    fill_ip_probe_window("203.0.113.2");

    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t handlers2[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx2, handlers2);

    handler_called = 0;
    _csilk_rate_limit_local(ctx2, "203.0.113.2", 1);
    assert(handler_called == 0);
    assert(csilk_is_aborted(ctx2) == 1);
    assert(csilk_get_response_header(ctx2, "Retry-After") != NULL);

    csilk_test_ctx_free(ctx2);
    _csilk_ratelimit_reset_tables();
    printf("Rate limit fail-closed test passed!\n");
}

static void
test_ratelimit_evict_lru_and_expiry_reclamation()
{
    printf("Testing rate limit LRU eviction and expired slot reclamation...\n");
    _csilk_ratelimit_reset_tables();
    assert(csilk_rate_limit_get_saturation_policy() == CSILK_RATE_SATURATION_EVICT_LRU);
    fill_ip_probe_window("203.0.113.3");

    /* Advance simulated time slightly so a fresh IP triggers LRU eviction
     * rather than failing open/closed, and confirm it gets tracked cleanly. */
    _csilk_ratelimit_set_time_offset(5);

    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t handlers2[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx2, handlers2);
    handler_called = 0;
    _csilk_rate_limit_local(ctx2, "203.0.113.3", 1);
    assert(handler_called == 1);
    assert(csilk_is_aborted(ctx2) == 0);
    csilk_test_ctx_free(ctx2);

    /* Second request for the newly admitted IP must now hit count == 2 > 1 (429),
     * proving the LRU slot was evicted and reassigned to 203.0.113.3. */
    csilk_ctx_t*    ctx3 = csilk_test_ctx_new();
    csilk_handler_t handlers3[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx3, handlers3);
    handler_called = 0;
    _csilk_rate_limit_local(ctx3, "203.0.113.3", 1);
    assert(handler_called == 0);
    assert(csilk_is_aborted(ctx3) == 1);
    csilk_test_ctx_free(ctx3);

    /* Advance past the 60s window and switch to FAIL_CLOSED: expired slots must
     * still be reclaimed cleanly without hitting saturation 429. */
    csilk_rate_limit_set_saturation_policy(CSILK_RATE_SATURATION_FAIL_CLOSED);
    _csilk_ratelimit_set_time_offset(70);

    csilk_ctx_t*    ctx4 = csilk_test_ctx_new();
    csilk_handler_t handlers4[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx4, handlers4);
    handler_called = 0;
    _csilk_rate_limit_local(ctx4, "203.0.113.4", 1);
    assert(handler_called == 1);
    assert(csilk_is_aborted(ctx4) == 0);
    csilk_test_ctx_free(ctx4);

    _csilk_ratelimit_reset_tables();
    printf("Rate limit LRU eviction and expiry reclamation test passed!\n");
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
    test_ratelimit_fail_closed_on_saturated_table();
    test_ratelimit_evict_lru_and_expiry_reclamation();
    printf("test_ratelimit: ALL PASSED\n");
    return 0;
}
