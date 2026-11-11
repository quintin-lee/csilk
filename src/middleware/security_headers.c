/**
 * @file security_headers.c
 * @brief Security response headers middleware.
 * @copyright MIT License
 */

#include "csilk/core/ctx/response.h"
#include "csilk/core/middleware.h"
#include "csilk/core/http/security_headers.h"

/* Default security header set (csilk#9b).  Overridable via
 * csilk_security_headers_middleware_ex(). */
static const csilk_security_header_t s_default_security_headers[] = {
    {"X-Frame-Options",        "DENY"                           },
    {"X-Content-Type-Options", "nosniff"                        },
    {"X-XSS-Protection",       "0"                              },
    {"Referrer-Policy",        "strict-origin-when-cross-origin"},
    {NULL,                     NULL                             }  /* terminator */
};

/**
 * @brief Apply a caller-supplied security-header array to the response.
 *
 * Each entry with a non-NULL value is set; an entry with a NULL value
 * is skipped.  A NULL @p headers array applies the built-in defaults.
 * The array must be terminated by an entry with a NULL name.
 */
static void
_csilk_security_headers_apply(csilk_ctx_t* c, const csilk_security_header_t* headers)
{
    if (!c) {
        return;
    }
    if (!headers) {
        headers = s_default_security_headers;
    }
    for (const csilk_security_header_t* h = headers; h->name != NULL; h++) {
        if (h->value != NULL) {
            csilk_set_header(c, h->name, h->value);
        }
        /* A NULL value omits this header. */
    }
}

/**
 * @brief Security headers middleware.
 *
 * Adds the default defensive response headers (see s_default_security_headers)
 * to prevent common web attacks:
 * - X-Frame-Options: DENY (prevent clickjacking)
 * - X-Content-Type-Options: nosniff (prevent MIME sniffing)
 * - X-XSS-Protection: 0 (disable legacy XSS filter)
 * - Referrer-Policy: strict-origin-when-cross-origin
 *
 * Note: Content-Security-Policy and HSTS are intentionally omitted as
 * they require application-specific configuration.  Use
 * csilk_security_headers_middleware_ex() to customize.
 */
void
csilk_security_headers_middleware(csilk_ctx_t* c)
{
    _csilk_security_headers_apply(c, s_default_security_headers);
    csilk_next(c);
}

/**
 * @brief Add caller-configurable security headers (csilk#9b).
 *
 * See the header comment in security_headers.h for the array contract.
 * Pass @p headers == NULL to apply the same defaults as
 * csilk_security_headers_middleware.
 */
void
csilk_security_headers_middleware_ex(csilk_ctx_t* c, const csilk_security_header_t* headers)
{
    _csilk_security_headers_apply(c, headers);
    csilk_next(c);
}
