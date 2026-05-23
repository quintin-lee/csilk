/**
 * @file auth.c
 * @brief Authentication middleware implementation.
 * MIT License
 */

#include <string.h>

#include "csilk.h"

/** @brief Authentication middleware handler.
 * @param c The request context.
 * @param validator The authentication validator function. */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator) {
  const char* token = csilk_get_header(c, "Authorization");
  if (!token || !validator(token)) {
    csilk_status(c, 401);
    csilk_abort(c);
  }
}
