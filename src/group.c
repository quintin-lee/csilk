/**
 * @file group.c
 * @brief Route group implementation.
 * MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "csilk.h"

/** @brief Route group structure. */
struct csilk_group_s {
  char* prefix;                  /**< URL prefix for this group. */
  csilk_router_t* router;          /**< Associated router instance. */
  csilk_handler_t* middlewares;    /**< Middleware handlers array. */
  size_t middleware_count;       /**< Number of middleware handlers. */
  csilk_group_t* parent;           /**< Parent group (NULL for root). */
};

/** @brief Internal helper to join URL paths.
 * @param p1 The first path component.
 * @param p2 The second path component.
 * @return A newly allocated joined path string. */
static char* join_path(const char* p1, const char* p2) {
  if (!p1 || *p1 == '\0') return strdup(p2 ? p2 : "/");
  if (!p2 || *p2 == '\0') return strdup(p1);

  size_t l1 = strlen(p1);
  while (l1 > 0 && p1[l1 - 1] == '/') l1--;

  const char* p2_start = p2;
  while (*p2_start == '/') p2_start++;

  size_t l2 = strlen(p2_start);

  char* res = malloc(l1 + l2 + 2);
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

csilk_group_t* csilk_group_new(csilk_router_t* router, const char* prefix) {
  csilk_group_t* group = calloc(1, sizeof(csilk_group_t));
  if (!group) return NULL;

  group->router = router;
  group->prefix = strdup(prefix ? prefix : "/");
  if (!group->prefix) {
    free(group);
    return NULL;
  }
  return group;
}

csilk_group_t* csilk_group_group(csilk_group_t* parent, const char* prefix) {
  if (!parent) return NULL;

  csilk_group_t* group = calloc(1, sizeof(csilk_group_t));
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

void csilk_group_use(csilk_group_t* group, csilk_handler_t handler) {
  if (!group) return;
  csilk_handler_t* new_middlewares =
      realloc(group->middlewares,
              (group->middleware_count + 1) * sizeof(csilk_handler_t));
  if (new_middlewares) {
    group->middlewares = new_middlewares;
    group->middlewares[group->middleware_count++] = handler;
  }
}

/** @brief Internal helper to gather all middleware handlers in the chain.
 * @param group The group.
 * @param handlers Pointer to store the handlers array.
 * @param count Pointer to store the handler count.
 * @return 0 on success, -1 on failure. */
static int gather_handlers(csilk_group_t* group, csilk_handler_t** handlers,
                           size_t* count) {
  if (group->parent) {
    if (gather_handlers(group->parent, handlers, count) != 0) {
      return -1;
    }
  }

  if (group->middleware_count > 0) {
    csilk_handler_t* new_handlers = realloc(
        *handlers, (*count + group->middleware_count) * sizeof(csilk_handler_t));
    if (!new_handlers) {
      return -1;
    }
    *handlers = new_handlers;
    memcpy(*handlers + *count, group->middlewares,
           group->middleware_count * sizeof(csilk_handler_t));
    *count += group->middleware_count;
  }
  return 0;
}

void csilk_group_add_route(csilk_group_t* group, const char* method,
                         const char* path, csilk_handler_t handler) {
  if (!group) return;

  char* full_path = join_path(group->prefix, path);
  if (!full_path) return;

  csilk_handler_t* handlers = NULL;
  size_t count = 0;

  if (gather_handlers(group, &handlers, &count) != 0) {
    free(full_path);
    free(handlers);
    return;
  }

  csilk_handler_t* new_handlers =
      realloc(handlers, (count + 1) * sizeof(csilk_handler_t));
  if (!new_handlers) {
    free(full_path);
    free(handlers);
    return;
  }
  handlers = new_handlers;
  handlers[count++] = handler;

  csilk_router_add(group->router, method, full_path, handlers, count);

  free(full_path);
  free(handlers);
}

void csilk_group_free(csilk_group_t* group) {
  if (!group) return;
  free(group->prefix);
  free(group->middlewares);
  free(group);
}
