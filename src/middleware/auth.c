/**
 * @file auth.c
 * @brief Token-based authentication middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "csilk/core/internal.h"

/**
 * @brief Token-based authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header and validates it
 * using the caller-provided validator callback. If validation fails, a
 * 401 Unauthorized response is sent with a WWW-Authenticate header, and the
 * request pipeline is aborted.
 *
 * @param c          The request context.
 * @param validator  Callback that receives the token string and returns
 *                   non-zero if the token is valid, zero otherwise.
 *
 * @note This middleware does NOT handle token parsing beyond stripping the
 *       "Bearer " prefix — use the JWT middleware for structured token
 *       verification. Should be registered early in the pipeline
 *       (after request_id and session, before route handlers).
 * @warning The validator must be stateless or thread-safe, as it may be
 *          invoked from multiple worker threads concurrently.
 */
void
csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator)
{
    CSILK_LOG_T("Auth: Authentication middleware triggered for request %p", (void*)c);
    /* Missing header or failing validator both yield 401.
     The validator receives the full Authorization value including "Bearer "
     prefix — it must strip it internally if needed. */
    const char* token = csilk_get_header(c, "Authorization");
    if (!token || !validator(token)) {
        CSILK_LOG_W("Auth: Authentication failed for request %p (Authorization header: %s)",
                    (void*)c,
                    token ? "present" : "missing");
        csilk_set_header(c, "WWW-Authenticate", "Bearer");
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Unauthorized");
        csilk_abort(c);
    } else {
        CSILK_LOG_D("Auth: Authentication successful for request %p", (void*)c);
    }
}
