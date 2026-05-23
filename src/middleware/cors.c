/**
 * @file cors.c
 * @brief CORS middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <string.h>
#include "csilk.h"
#include "csilk_internal.h"

/** @brief CORS middleware — sets cross-origin headers and handles preflight. */
void csilk_cors_middleware(csilk_ctx_t* c, const csilk_cors_config_t* config) {
  if (!c || !config) {
    if (c) csilk_next(c);
    return;
  }

  csilk_set_header(c, "Access-Control-Allow-Origin", config->allow_origin);
  csilk_set_header(c, "Access-Control-Allow-Methods", config->allow_methods);
  csilk_set_header(c, "Access-Control-Allow-Headers", config->allow_headers);

  if (config->allow_credentials)
    csilk_set_header(c, "Access-Control-Allow-Credentials", "true");

  if (config->max_age > 0) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", config->max_age);
    if (n > 0 && (size_t)n < sizeof(buf))
      csilk_set_header(c, "Access-Control-Max-Age", buf);
  }

  const char* req_method = csilk_get_header(c, "Access-Control-Request-Method");
  if (csilk_get_method(c) && strcmp(csilk_get_method(c), "OPTIONS") == 0 && req_method) {
    csilk_string(c, 204, "");
    csilk_abort(c);
    return;
  }
  csilk_next(c);
}
