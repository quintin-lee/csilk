#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gin.h"

// Basic CSRF middleware (Stateless, token-based)
// In a real implementation, this would involve sessions or signed cookies
void gin_csrf_middleware(gin_ctx_t* c) {
    if (c->request.method && (strcmp(c->request.method, "GET") == 0 || 
                             strcmp(c->request.method, "HEAD") == 0 ||
                             strcmp(c->request.method, "OPTIONS") == 0)) {
        gin_next(c);
        return;
    }

    const char* token = gin_get_header(c, "X-CSRF-Token");
    if (!token) {
        gin_json_error(c, 403, "Forbidden: CSRF token missing");
        gin_abort(c);
        return;
    }

    // For demonstration, we check against a static secret or cookie
    // Real-world: compare token with session-stored token
    const char* cookie = gin_get_header(c, "Cookie");
    if (cookie && strstr(cookie, token)) {
        gin_next(c);
    } else {
        gin_json_error(c, 403, "Forbidden: Invalid CSRF token");
        gin_abort(c);
    }
}
