/**
 * @file url_parser.c
 * @brief URL parsing implementation.
 * @license MIT
 */

#include <stdlib.h>
#include <string.h>
...

/** @brief Internal helper to split URL path and query.
 * @param url The URL string.
 * @param path Pointer to store the path string.
 * @param query Pointer to store the query string. */
void gin_split_url(const char* url, char** path, char** query) {
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

/** @brief Internal helper to parse query string.
 * @param c The request context.
 * @param query_string The query string to parse. */
void gin_parse_query(gin_ctx_t* c, const char* query_string) {
  if (!query_string || *query_string == '\0') return;

  char* qs = strdup(query_string);
  if (!qs) return;

  gin_header_t** tail = &c->request.query_params;
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
      gin_header_t* new_q = malloc(sizeof(gin_header_t));
      if (new_q) {
        new_q->key = strdup(key);
        new_q->value = strdup(value);
        new_q->next = NULL;

        if (new_q->key && new_q->value) {
          *tail = new_q;
          tail = &new_q->next;
        } else {
          free(new_q->key);
          free(new_q->value);
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
