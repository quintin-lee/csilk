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
    test_security_headers_default();
    test_security_headers_custom();
    test_security_headers_null_array();

    printf("test_keyed_ratelimit_security_headers: ALL PASSED\n");
    return 0;
}
