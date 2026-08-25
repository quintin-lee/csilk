#pragma once

#include "csilk/core/types.h"

/**
 * @brief Add security response headers to the current request.
 *
 * Sets X-Frame-Options, X-Content-Type-Options, X-XSS-Protection,
 * and Referrer-Policy headers. Must be called before csilk_next().
 *
 * @param c The request context.
 */
void csilk_security_headers_middleware(csilk_ctx_t* c);
