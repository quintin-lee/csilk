/**
 * @file csrf.c
 * @brief Stateless CSRF protection middleware implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk.h"
#include "csilk_internal.h"

/** @brief Stateless CSRF protection middleware (cookie + header token
 * comparison). */
void csilk_csrf_middleware(csilk_ctx_t* c) {
  if (csilk_get_method(c) && (strcmp(csilk_get_method(c), "GET") == 0 ||
                              strcmp(csilk_get_method(c), "HEAD") == 0 ||
                              strcmp(csilk_get_method(c), "OPTIONS") == 0)) {
    // Set CSRF cookie on safe methods so frontend can read it
    const char* existing = csilk_get_cookie(c, "csrf_token");
    if (!existing) {
      char token_buf[33];
      if (csilk_csrf_generate_token(token_buf, sizeof(token_buf)) == 0) {
        csilk_set_cookie(c, "csrf_token", token_buf, 86400, "/", NULL, 0, 1);
      }
    }
    csilk_next(c);
    return;
  }

  const char* token = csilk_get_header(c, "X-CSRF-Token");
  if (!token) {
    csilk_json_error(c, CSILK_STATUS_FORBIDDEN,
                     "Forbidden: CSRF token missing");
    csilk_abort(c);
    return;
  }

  const char* cookie_token = csilk_get_cookie(c, "csrf_token");
  if (cookie_token && strcmp(cookie_token, token) == 0) {
    csilk_next(c);
  } else {
    csilk_json_error(c, CSILK_STATUS_FORBIDDEN,
                     "Forbidden: Invalid CSRF token");
    csilk_abort(c);
  }
}

/** @brief Generate a cryptographically random CSRF token. */
int csilk_csrf_generate_token(char* buf, size_t buf_size) {
  if (!buf || buf_size < 33) return -1;

  /* use /dev/urandom for cryptographically random bytes */
  FILE* fp = fopen("/dev/urandom", "rb");
  if (!fp) {
    /* fallback: use time+pid as weak entropy (better than nothing) */
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    snprintf(buf, buf_size, "%08x%08x%08x%08x", rand_r(&seed), rand_r(&seed),
             rand_r(&seed), rand_r(&seed));
  } else {
    uint8_t random[16];
    if (fread(random, 1, sizeof(random), fp) != sizeof(random)) {
      fclose(fp);
      return -1;
    }
    fclose(fp);
    snprintf(buf, buf_size,
             "%02x%02x%02x%02x%02x%02x%02x%02x"
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3], random[4], random[5],
             random[6], random[7], random[8], random[9], random[10], random[11],
             random[12], random[13], random[14], random[15]);
  }
  return 0;
}
