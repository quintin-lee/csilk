#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/http/security_headers.h"

/*
 * P4 regression tests for csilk#9a (keyed rate limiting) and
 * csilk#9b (configurable security headers).
 *
 * Before the fix:
 *   - csilk_rate_limit_middleware only throttled by client IP; a business
 *     key (user_id / tenant_id) had no first-class support.
 *   - csilk_security_headers_middleware set a fixed 4-header set; there
 *     was no way to add a CSP / HSTS or override a default.
 *
 * After the fix:
 *   - csilk_rate_limit_keyed(c, limit, key) throttles by an explicit
 *     caller-supplied key, reusing the same lockless sliding-window table.
 *   - csilk_security_headers_middleware_ex(c, headers) applies a
 *     caller-supplied header array; a NULL array keeps the defaults.
 */

static int handler_called = 0;

static void
test_handler(csilk_ctx_t* c)
{
    (void)c;
    handler_called++;
}

/* Helper: run a single keyed-rate-limit decision on a fresh ctx and report
 * whether the request passed (handler was invoked) or was blocked.  A fresh
 * ctx is needed because csilk_next advances handler_index; the global IP
 * table is independent of the ctx, so reuse is safe. */
static int
run_keyed(const char* key, int limit)
{
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    handler_called = 0;
    csilk_rate_limit_keyed(ctx, limit, key);
    int passed = (handler_called == 1 && csilk_is_aborted(ctx) == 0);
    csilk_test_ctx_free(ctx);
    return passed;
}

/* ---- csilk#9a: keyed rate limiting ---- */

static const char* g_custom_key = "tenant_alpha";

static const char*
extract_custom_key(csilk_ctx_t* c)
{
    (void)c;
    return g_custom_key;
}

static int
run_limiter(const csilk_rate_limiter_config_t* cfg, const char* path)
{
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);
    if (path) {
        csilk_test_ctx_set_request(ctx, "GET", path);
    }

    handler_called = 0;
    csilk_rate_limiter_middleware(ctx, cfg);
    int passed = (handler_called == 1 && csilk_is_aborted(ctx) == 0);
    csilk_test_ctx_free(ctx);
    return passed;
}

static uint32_t
test_djb2_key_hash(const char* s, uint32_t table_size)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*s++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % table_size;
}

static void
fill_key_probe_window(const csilk_rate_limiter_config_t* cfg, const char* target_key)
{
    enum { KEY_TABLE_SIZE = 8192, PROBE_WIN = 512 };
    uint32_t target_h = test_djb2_key_hash(target_key, KEY_TABLE_SIZE);
    char     keys[PROBE_WIN][32];
    int      filled[PROBE_WIN] = {0};
    int      remaining = PROBE_WIN;

    for (uint32_t n = 0; remaining > 0; n++) {
        char candidate[32];
        snprintf(candidate, sizeof(candidate), "tf_%u", n);
        uint32_t h = test_djb2_key_hash(candidate, KEY_TABLE_SIZE);
        uint32_t off = (h + KEY_TABLE_SIZE - target_h) % KEY_TABLE_SIZE;
        if (off < PROBE_WIN && !filled[off]) {
            snprintf(keys[off], sizeof(keys[off]), "%s", candidate);
            filled[off] = 1;
            remaining--;
        }
    }

    for (int i = 0; i < PROBE_WIN; i++) {
        g_custom_key = keys[i];
        assert(run_limiter(cfg, "/v1/chat") == 1);
    }
}

static void
test_rate_limiter_keyed_policies(void)
{
    printf("Testing keyed rate limiter saturation, expiry & path filters...\n");
    _csilk_ratelimit_reset_tables();

    /* Custom key with 5s window: 1st allowed, 2nd blocked, +6s allowed again. */
    csilk_rate_limiter_config_t custom_cfg = {
        .key_type = CSILK_RATE_KEY_CUSTOM,
        .limit = 1,
        .window_ms = 5000,
        .key_fn = extract_custom_key,
        .saturation_policy = CSILK_RATE_SATURATION_EVICT_LRU,
    };
    g_custom_key = "tenant_window_test";
    assert(run_limiter(&custom_cfg, "/v1/chat") == 1);
    assert(run_limiter(&custom_cfg, "/v1/chat") == 0);
    _csilk_ratelimit_set_time_offset(6);
    assert(run_limiter(&custom_cfg, "/v1/chat") == 1);

    /* Saturate the 512-slot probe window for tenant_overflow_closed under
     * FAIL_CLOSED, verify rejection, FAIL_OPEN allowance, and EVICT_LRU. */
    _csilk_ratelimit_reset_tables();
    fill_key_probe_window(&custom_cfg, "tenant_overflow_closed");

    _csilk_ratelimit_set_time_offset(1);
    custom_cfg.saturation_policy = CSILK_RATE_SATURATION_FAIL_CLOSED;
    g_custom_key = "tenant_overflow_closed";
    assert(run_limiter(&custom_cfg, "/v1/chat") == 0);

    custom_cfg.saturation_policy = CSILK_RATE_SATURATION_FAIL_OPEN;
    g_custom_key = "tenant_overflow_closed";
    assert(run_limiter(&custom_cfg, "/v1/chat") == 1);

    custom_cfg.saturation_policy = CSILK_RATE_SATURATION_EVICT_LRU;
    g_custom_key = "tenant_overflow_closed";
    assert(run_limiter(&custom_cfg, "/v1/chat") == 1);
    assert(run_limiter(&custom_cfg, "/v1/chat") == 0);

    /* Advance past 5s window: expired slots must be reclaimed even under FAIL_CLOSED. */
    _csilk_ratelimit_set_time_offset(10);
    custom_cfg.saturation_policy = CSILK_RATE_SATURATION_FAIL_CLOSED;
    g_custom_key = "tenant_overflow_closed_2";
    assert(run_limiter(&custom_cfg, "/v1/chat") == 1);

    /* IP_PATH with exclude_prefixes & match_paths. */
    _csilk_ratelimit_reset_tables();
    const char*                 excludes[] = {"/health", NULL};
    const char*                 matches[] = {"/api/login", NULL};
    csilk_rate_limiter_config_t path_cfg = {
        .key_type = CSILK_RATE_KEY_IP_PATH,
        .limit = 1,
        .window_ms = 5000,
        .match_paths = matches,
        .exclude_prefixes = excludes,
        .saturation_policy = CSILK_RATE_SATURATION_EVICT_LRU,
    };
    assert(run_limiter(&path_cfg, "/health/live") == 1);
    assert(run_limiter(&path_cfg, "/health/live") == 1);
    assert(run_limiter(&path_cfg, "/other") == 1);
    assert(run_limiter(&path_cfg, "/other") == 1);
    assert(run_limiter(&path_cfg, "/api/login") == 1);
    assert(run_limiter(&path_cfg, "/api/login") == 0);

    _csilk_ratelimit_reset_tables();
    printf("  passed\n");
}

static void
test_keyed_rate_limit_basic(void)
{
    printf("Testing keyed rate limit basic (csilk#9a)...\n");
    /* Distinct keys behave independently. */
    assert(run_keyed("user_a_1001", 3) == 1);
    assert(run_keyed("user_b_2002", 3) == 1);
    assert(run_keyed("user_c_3003", 10) == 1);
    printf("  passed\n");
}

/* ---- csilk#9b: configurable security headers ---- */

static int
header_present(csilk_ctx_t* c, const char* name, const char* expected)
{
    const char* actual = csilk_get_response_header(c, name);
    if (!actual) {
        return 0;
    }
    return strcmp(actual, expected) == 0;
}

static void
test_security_headers_default(void)
{
    printf("Testing security headers default (csilk#9b)...\n");
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_security_headers_middleware(ctx);
    assert(header_present(ctx, "X-Frame-Options", "DENY"));
    assert(header_present(ctx, "X-Content-Type-Options", "nosniff"));
    assert(header_present(ctx, "X-XSS-Protection", "0"));
    assert(header_present(ctx, "Referrer-Policy", "strict-origin-when-cross-origin"));

    csilk_test_ctx_free(ctx);
    printf("  passed\n");
}

static void
test_security_headers_custom(void)
{
    printf("Testing security headers custom (csilk#9b)...\n");
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    static const csilk_security_header_t custom[] = {
        {"X-Frame-Options",           "SAMEORIGIN"        },
        {"Content-Security-Policy",   "default-src 'self'"},
        {"Strict-Transport-Security", "max-age=31536000"  },
        {"X-XSS-Protection",          NULL                }, /* omit */
        {NULL,                        NULL                }  /* terminator */
    };

    csilk_security_headers_middleware_ex(ctx, custom);
    assert(header_present(ctx, "X-Frame-Options", "SAMEORIGIN"));
    assert(header_present(ctx, "Content-Security-Policy", "default-src 'self'"));
    assert(header_present(ctx, "Strict-Transport-Security", "max-age=31536000"));
    /* X-XSS-Protection must NOT be set (value was NULL → omitted). */
    assert(!header_present(ctx, "X-XSS-Protection", "0"));

    csilk_test_ctx_free(ctx);
    printf("  passed\n");
}

static void
test_security_headers_null_array(void)
{
    printf("Testing security headers NULL-array == defaults (csilk#9b)...\n");
    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {test_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_security_headers_middleware_ex(ctx, NULL);
    assert(header_present(ctx, "X-Frame-Options", "DENY"));
    assert(header_present(ctx, "X-Content-Type-Options", "nosniff"));

    csilk_test_ctx_free(ctx);
    printf("  passed\n");
}

int
main(void)
{
    test_keyed_rate_limit_basic();
    test_rate_limiter_keyed_policies();
    test_security_headers_default();
    test_security_headers_custom();
    test_security_headers_null_array();

    printf("test_keyed_ratelimit_security_headers: ALL PASSED\n");
    return 0;
}
