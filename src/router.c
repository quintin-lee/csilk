#include "gin.h"
#include <stdlib.h>
#include <string.h>

struct gin_router_node_s {
    char *path;
    char *method;
    gin_handler_t handler;
    struct gin_router_node_s *next;
};

gin_router_t* gin_router_new() {
    gin_router_t *r = malloc(sizeof(gin_router_t));
    if (r) {
        r->root = NULL;
    }
    return r;
}

void gin_router_add(gin_router_t *r, const char *method, const char *path, gin_handler_t handler) {
    gin_router_node_t *node = malloc(sizeof(gin_router_node_t));
    if (node) {
        node->path = strdup(path);
        node->method = strdup(method);
        node->handler = handler;
        node->next = r->root;
        r->root = node;
    }
}

gin_handler_t gin_router_match(gin_router_t *r, const char *method, const char *path) {
    gin_router_node_t *curr = r->root;
    while(curr) {
        if (strcmp(curr->method, method) == 0 && strcmp(curr->path, path) == 0) {
            return curr->handler;
        }
        curr = curr->next;
    }
    return NULL;
}

void gin_router_free(gin_router_t *r) {
    gin_router_node_t *curr = r->root;
    while(curr) {
        gin_router_node_t *next = curr->next;
        free(curr->path);
        free(curr->method);
        free(curr);
        curr = next;
    }
    free(r);
}
