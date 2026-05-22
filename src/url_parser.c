#include "gin.h"
#include <string.h>
#include <stdlib.h>

void gin_split_url(const char *url, char **path, char **query) {
    if (!url) {
        *path = NULL;
        *query = NULL;
        return;
    }

    const char *qmark = strchr(url, '?');
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
        *query = NULL;
    }
}

static void add_query_param(gin_ctx_t *c, const char *key, const char *value) {
    gin_header_t *new_q = malloc(sizeof(gin_header_t));
    if (!new_q) return;

    new_q->key = strdup(key);
    new_q->value = strdup(value);
    new_q->next = NULL;

    if (!new_q->key || !new_q->value) {
        free(new_q->key);
        free(new_q->value);
        free(new_q);
        return;
    }

    if (!c->request.query_params) {
        c->request.query_params = new_q;
    } else {
        gin_header_t *curr = c->request.query_params;
        while (curr->next) {
            curr = curr->next;
        }
        curr->next = new_q;
    }
}

void gin_parse_query(gin_ctx_t *c, const char *query_string) {
    if (!query_string || *query_string == '\0') return;

    char *qs = strdup(query_string);
    if (!qs) return;

    char *saveptr1, *saveptr2;
    char *pair = strtok_r(qs, "&", &saveptr1);
    while (pair != NULL) {
        char *key = strtok_r(pair, "=", &saveptr2);
        char *value = strtok_r(NULL, "=", &saveptr2);
        
        if (key) {
            add_query_param(c, key, value ? value : "");
        }
        pair = strtok_r(NULL, "&", &saveptr1);
    }
    free(qs);
}
