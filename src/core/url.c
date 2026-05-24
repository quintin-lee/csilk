/**
 * @file url.c
 * @brief URL parsing implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Helper to convert hex character to integer. */
static int hex_to_int(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** @brief URL decode a string in-place. */
size_t csilk_url_decode(char* str) {
  if (!str) return 0;
  char* src = str;
  char* dst = str;
  while (*src) {
    if (*src == '%' && isxdigit(src[1]) && isxdigit(src[2])) {
      int hi = hex_to_int(src[1]);
      int lo = hex_to_int(src[2]);
      if (hi >= 0 && lo >= 0) {
        *dst++ = (char)((hi << 4) | lo);
        src += 3;
        continue;
      }
    } else if (*src == '+') {
      *dst++ = ' ';
    } else {
      *dst++ = *src;
    }
    src++;
  }
  *dst = '\0';
  return dst - str;
}

/** @brief Split a URL into path and query string components. */
void csilk_split_url(const char* url, char** path, char** query) {
  *path = NULL;
  *query = NULL;
  if (!url) return;

  const char* qmark = strchr(url, '?');
  if (qmark) {
    size_t path_len = qmark - url;
    *path = malloc(path_len + 1);
    if (!*path) return;

    memcpy(*path, url, path_len);
    (*path)[path_len] = '\0';
    csilk_url_decode(*path);

    *query = strdup(qmark + 1);
    if (!*query) {
      free(*path);
      *path = NULL;
    }
  } else {
    *path = strdup(url);
    if (*path) csilk_url_decode(*path);
  }
}
