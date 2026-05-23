/**
 * @file url.c
 * @brief URL parsing implementation.
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>
#include "csilk.h"

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

    *query = strdup(qmark + 1);
    if (!*query) {
      free(*path);
      *path = NULL;
    }
  } else {
    *path = strdup(url);
  }
}
