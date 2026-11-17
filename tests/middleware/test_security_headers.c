/**
 * @file test_security_headers.c
 * @brief Standalone regression tests for csilk#9b (configurable security headers).
 *
 * Verifies that:
 *   - csilk_security_headers_middleware() sets the default 4-header set.
 *   - csilk_security_headers_middleware_ex(ctx, custom) applies a
 *     caller-supplied array (NULL values omit the header).
 *   - csilk_security_headers_middleware_ex(ctx, NULL) falls back to
 *     the default set.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/http/security_headers.h"

static int handler_called = 0;

static void
test_handler(csilk_ctx_t* c)
{
    (void)c;
    handler_called++;
}

/* Helper: check that a response header is present with the expected value. */
static int
header_present(csilk_ctx_t* c, const char* name, const char* expected)
{
    const char* actual = csilk_get_response_header(c, name);
    if (!actual) {
        return 0;
    }
    return strcmp(actual, expected) == 0;
}

/* --- csilk#9b: configurable security headers --- */

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
    test_security_headers_default();
    test_security_headers_custom();
    test_security_headers_null_array();

    printf("test_security_headers: ALL PASSED\n");
    return 0;
}
