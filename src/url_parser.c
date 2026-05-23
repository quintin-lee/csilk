/**
 * @file url_parser.c
 * @brief URL parsing implementation.
 * MIT License
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
    if (*path) {
      memcpy(*path, url, path_len);
      (*path)[path_len] = '\0';
    }
    *query = strdup(qmark + 1);
  } else {
    *path = strdup(url);
  }
}

void csilk_parse_query(csilk_ctx_t* c, const char* query_string) {
  if (!query_string || *query_string == '\0') return;

  char* qs = strdup(query_string);
  if (!qs) return;

  csilk_header_t** tail = &c->request.query_params;
  // Fast-forward to end if already populated
  while (*tail) {
    tail = &((*tail)->next);
  }

  char* pos = qs;
  while (pos && *pos) {
    char* amp = strchr(pos, '&');
    if (amp) *amp = '\0';

    char* eq = strchr(pos, '=');
    char* key = pos;
    char* value = "";

    if (eq) {
      *eq = '\0';
      value = eq + 1;
    }

    // Only add if key is not empty (ignores =val and &&)
    if (*key != '\0') {
      csilk_header_t* new_q = malloc(sizeof(csilk_header_t));
      if (new_q) {
        new_q->key = strdup(key);
        new_q->value = strdup(value);
        new_q->next = NULL;

        if (new_q->key && new_q->value) {
          *tail = new_q;
          tail = &new_q->next;
        } else {
          if (new_q->key) free(new_q->key);
          if (new_q->value) free(new_q->value);
          free(new_q);
        }
      }
    }

    if (amp)
      pos = amp + 1;
    else
      pos = NULL;
  }
  free(qs);
}
