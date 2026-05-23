/**
 * @file csrf.c
 * @brief Stateless CSRF protection middleware implementation.
 * MIT License
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "csilk.h"

/** @brief Stateless CSRF protection middleware.
 * Checks the X-CSRF-Token header for non-safe methods (POST, PUT, DELETE, PATCH).
 * GET, HEAD, and OPTIONS are allowed without a token.
 * @param c The request context. */
void csilk_csrf_middleware(csilk_ctx_t* c) {
    if (c->request.method && (strcmp(c->request.method, "GET") == 0 || 
                             strcmp(c->request.method, "HEAD") == 0 ||
                             strcmp(c->request.method, "OPTIONS") == 0)) {
        csilk_next(c);
        return;
    }

    const char* token = csilk_get_header(c, "X-CSRF-Token");
    if (!token) {
        csilk_json_error(c, 403, "Forbidden: CSRF token missing");
        csilk_abort(c);
        return;
    }

    // Fix: Use csilk_get_cookie for exact matching instead of strstr
    const char* cookie_token = csilk_get_cookie(c, "csrf_token");
    if (cookie_token && strcmp(cookie_token, token) == 0) {
        csilk_next(c);
    } else {
        csilk_json_error(c, 403, "Forbidden: Invalid CSRF token");
        csilk_abort(c);
    }
}
