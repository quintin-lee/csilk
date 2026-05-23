/**
 * @file auth.c
 * @brief Token-based authentication middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "csilk.h"
#include "csilk_internal.h"

/** @brief Token-based authentication middleware. */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator) {
  const char* token = csilk_get_header(c, "Authorization");
  if (!token || !validator(token)) {
    csilk_status(c, 401);
    csilk_abort(c);
  }
}
