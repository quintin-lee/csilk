#include "gin.h"
#include <stdlib.h>
#include <string.h>

struct gin_group_s {
    char *prefix;
    gin_router_t *router;
    gin_handler_t *middlewares;
    size_t middleware_count;
    gin_group_t *parent;
};

static char* join_path(const char *p1, const char *p2) {
    if (!p1 || *p1 == '\0') return strdup(p2 ? p2 : "/");
    if (!p2 || *p2 == '\0') return strdup(p1);

    size_t l1 = strlen(p1);
    while (l1 > 0 && p1[l1-1] == '/') l1--;
    
    const char *p2_start = p2;
    while (*p2_start == '/') p2_start++;
    
    size_t l2 = strlen(p2_start);
    
    char *res = malloc(l1 + l2 + 2);
    if (!res) return NULL;
    
    if (l1 > 0) {
        memcpy(res, p1, l1);
        res[l1] = '/';
        memcpy(res + l1 + 1, p2_start, l2);
        res[l1 + 1 + l2] = '\0';
    } else {
        res[0] = '/';
        memcpy(res + 1, p2_start, l2);
        res[1 + l2] = '\0';
    }
    return res;
}

gin_group_t* gin_group_new(gin_router_t *router, const char *prefix) {
    gin_group_t *group = calloc(1, sizeof(gin_group_t));
    if (!group) return NULL;
    
    group->router = router;
    group->prefix = strdup(prefix ? prefix : "/");
    if (!group->prefix) {
        free(group);
        return NULL;
    }
    return group;
}

gin_group_t* gin_group_group(gin_group_t *parent, const char *prefix) {
    if (!parent) return NULL;
    
    gin_group_t *group = calloc(1, sizeof(gin_group_t));
    if (!group) return NULL;
    
    group->parent = parent;
    group->router = parent->router;
    group->prefix = join_path(parent->prefix, prefix);
    
    if (!group->prefix) {
        free(group);
        return NULL;
    }
    return group;
}

void gin_group_use(gin_group_t *group, gin_handler_t handler) {
    if (!group) return;
    gin_handler_t *new_middlewares = realloc(group->middlewares, (group->middleware_count + 1) * sizeof(gin_handler_t));
    if (new_middlewares) {
        group->middlewares = new_middlewares;
        group->middlewares[group->middleware_count++] = handler;
    }
}

static int gather_handlers(gin_group_t *group, gin_handler_t **handlers, size_t *count) {
    if (group->parent) {
        if (gather_handlers(group->parent, handlers, count) != 0) {
            return -1;
        }
    }
    
    if (group->middleware_count > 0) {
        gin_handler_t *new_handlers = realloc(*handlers, (*count + group->middleware_count) * sizeof(gin_handler_t));
        if (!new_handlers) {
            return -1;
        }
        *handlers = new_handlers;
        memcpy(*handlers + *count, group->middlewares, group->middleware_count * sizeof(gin_handler_t));
        *count += group->middleware_count;
    }
    return 0;
}

void gin_group_add_route(gin_group_t *group, const char *method, const char *path, gin_handler_t handler) {
    if (!group) return;
    
    char *full_path = join_path(group->prefix, path);
    if (!full_path) return;

    gin_handler_t *handlers = NULL;
    size_t count = 0;
    
    if (gather_handlers(group, &handlers, &count) != 0) {
        free(full_path);
        free(handlers);
        return;
    }
    
    gin_handler_t *new_handlers = realloc(handlers, (count + 1) * sizeof(gin_handler_t));
    if (!new_handlers) {
        free(full_path);
        free(handlers);
        return;
    }
    handlers = new_handlers;
    handlers[count++] = handler;

    gin_router_add(group->router, method, full_path, handlers, count);
    
    free(full_path);
    free(handlers);
}

void gin_group_free(gin_group_t *group) {
    if (!group) return;
    free(group->prefix);
    free(group->middlewares);
    free(group);
}
