/**
 * @file auth.c
 * @brief Token-based authentication middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
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
 *       verification.
 * @warning The validator must be stateless or thread-safe, as it may be
 *          invoked from multiple worker threads concurrently.
 */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator) {
  const char* token = csilk_get_header(c, "Authorization");
  if (!token || !validator(token)) {
    csilk_set_header(c, "WWW-Authenticate", "Bearer");
    csilk_status(c, CSILK_STATUS_UNAUTHORIZED);
    csilk_abort(c);
  }
}
