/**
 * @file security_headers.c
 * @brief Security response headers middleware.
 * @copyright MIT License
 */

#include "csilk/core/ctx/response.h"

/**
 * @brief Security headers middleware.
 *
 * Adds defensive response headers to prevent common web attacks:
 * - X-Frame-Options: DENY (prevent clickjacking)
 * - X-Content-Type-Options: nosniff (prevent MIME sniffing)
 * - X-XSS-Protection: 0 (disable legacy XSS filter)
 * - Referrer-Policy: strict-origin-when-cross-origin
 *
 * Note: Content-Security-Policy and HSTS are intentionally omitted as
 * they require application-specific configuration.
 */
void
csilk_security_headers_middleware(csilk_ctx_t* c)
{
    csilk_set_header(c, "X-Frame-Options", "DENY");
    csilk_set_header(c, "X-Content-Type-Options", "nosniff");
    csilk_set_header(c, "X-XSS-Protection", "0");
    csilk_set_header(c, "Referrer-Policy", "strict-origin-when-cross-origin");

    csilk_next(c);
}
