#pragma once

#include "csilk/core/types.h"

/**
 * @brief A configurable security-header entry.
 *
 * Passed to csilk_security_headers_middleware_ex() (csilk#9b).  A NULL
 * @p name terminates the array; a NULL @p value means "omit this header".
 */
typedef struct {
    const char* name;
    const char* value;
} csilk_security_header_t;

/**
 * @brief Add the built-in default security headers to the current request.
 *
 * Sets X-Frame-Options, X-Content-Type-Options, X-XSS-Protection,
 * and Referrer-Policy headers. Must be called before csilk_next().
 *
 * @param c The request context.
 */
void csilk_security_headers_middleware(csilk_ctx_t* c);

/**
 * @brief Add caller-configurable security headers (csilk#9b).
 *
 * Unlike csilk_security_headers_middleware (fixed 4 defaults), this lets
 * the caller supply its own header set — e.g. to add a Content-Security-Policy
 * or HSTS, override X-Frame-Options to SAMEORIGIN, or drop a header.  The
 * array must be terminated by an entry with a NULL name.  An entry with a
 * NULL value omits that header entirely.
 *
 * @param c       The request context.
 * @param headers NUL-terminated csilk_security_header_t array.
 *                Pass NULL to apply the same defaults as
 *                csilk_security_headers_middleware.
 */
void csilk_security_headers_middleware_ex(csilk_ctx_t* c, const csilk_security_header_t* headers);
