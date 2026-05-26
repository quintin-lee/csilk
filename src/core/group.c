/**
 * @file group.c
 * @brief Route group implementation.
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Route group — holds a URL prefix, middleware chain, and optional
 * parent group for nesting. */
#define CSILK_GROUP_MW_INIT_CAP 4

struct csilk_group_s {
  char* prefix;                 /**< URL prefix for this group. */
  csilk_router_t* router;       /**< Associated router instance. */
  csilk_handler_t* middlewares; /**< Middleware handlers array. */
  size_t middleware_count;      /**< Number of middleware handlers. */
  size_t middleware_capacity;   /**< Allocated capacity of middlewares. */
  csilk_group_t* parent;        /**< Parent group (NULL for root). */
};

/** @brief Internal: join two URL path components with a single '/' separator.
 *
 * Handles leading/trailing slashes on both components to produce a clean
 * concatenated path. For example, join_path("/api/", "/v1") -> "/api/v1".
 *
 * @param p1 First path component (may be empty or NULL).
 * @param p2 Second path component (may be empty or NULL).
 * @return A newly heap-allocated joined path string. Caller must free().
 * @note Returns strdup("/") if both components are NULL or empty. */
static char* join_path(const char* p1, const char* p2) {
  if (!p1 || *p1 == '\0') {
    return strdup(p2 ? p2 : "/");
  }
  if (!p2 || *p2 == '\0') {
    return strdup(p1);
  }

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

/** @brief Create a new root route group with the given URL prefix.
 *
 * Root groups are attached directly to a router. All routes added to this
 * group will be prefixed with @p prefix.
 *
 * @param router The router instance this group belongs to.
 * @param prefix URL prefix for all routes in this group (e.g., "/api/v1").
 *               Pass NULL or "/" for no prefix.
 * @return A new csilk_group_t, or NULL on allocation failure.
 * @note The group must be freed with csilk_group_free(). */
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

/** @brief Create a child subgroup nested under a parent group.
 *
 * The child inherits the parent's router and its prefix is joined with the
 * parent's prefix (e.g., parent="/api", child="/v1" -> combined prefix
 * "/api/v1").
 *
 * @param parent The parent group (cannot be NULL).
 * @param prefix URL prefix for this subgroup (e.g., "/v1").
 * @return A new csilk_group_t, or NULL on failure.
 * @note The child must be freed separately with csilk_group_free() — freeing
 *       the parent does NOT free its children. */
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

/** @brief Register a middleware handler that applies to all routes in this
 * group.
 *
 * Middleware handlers are executed before route handlers in the order they
 * are registered. The internal middleware array grows dynamically (doubling
 * capacity) as needed.
 *
 * @param group   The route group.
 * @param handler Middleware handler function. Receives the request context
 *                and should call csilk_next() to pass control forward.
 * @note Middleware from parent groups is automatically inherited by child
 *       groups and prepended before child middleware. */
void csilk_group_use(csilk_group_t* group, csilk_handler_t handler) {
  if (!group) return;
  if (group->middleware_count >= group->middleware_capacity) {
    size_t new_cap = group->middleware_capacity ? group->middleware_capacity * 2
                                                : CSILK_GROUP_MW_INIT_CAP;
    csilk_handler_t* new_mw =
        realloc(group->middlewares, new_cap * sizeof(csilk_handler_t));
    if (!new_mw) return;
    group->middlewares = new_mw;
    group->middleware_capacity = new_cap;
  }
  group->middlewares[group->middleware_count++] = handler;
}

/** @brief Internal: recursively collect all middleware handlers from a group
 *        and its ancestors into a flat array.
 *
 * Traverses the parent chain upward first, then appends the current group's
 * middleware. This ensures parent middleware runs before child middleware.
 * The handlers array is dynamically grown via realloc().
 *
 * @param group    The leaf group.
 * @param handlers [in/out] Pointer to the handlers array (realloc'd as needed).
 * @param count    [in/out] Number of handlers collected so far.
 * @return 0 on success, -1 on realloc failure.
 * @note The caller must free the returned handlers array with free(). */
static int gather_handlers(csilk_group_t* group, csilk_handler_t** handlers,
                           size_t* count) {
  if (group->parent) {
    if (gather_handlers(group->parent, handlers, count) != 0) {
      return -1;
    }
  }

  if (group->middleware_count > 0) {
    csilk_handler_t* new_handlers =
        realloc(*handlers,
                (*count + group->middleware_count) * sizeof(csilk_handler_t));
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

/** @brief Register a route on this group with a single handler.
 *
 * The route's path is automatically prefixed with the group's prefix. The
 * handler is wrapped in a 1-element array and passed to
 * csilk_group_add_handlers() which combines group middleware.
 *
 * @param group   The route group.
 * @param method  HTTP method (e.g., "GET", "POST").
 * @param path    Path relative to the group prefix (e.g., "/users").
 * @param handler The route handler function. */
void csilk_group_add_route(csilk_group_t* group, const char* method,
                           const char* path, csilk_handler_t handler) {
  csilk_handler_t handlers[] = {handler};
  csilk_group_add_handlers(group, method, path, handlers, 1);
}

/** @brief Register a route with OpenAPI metadata (input/output types, summary,
 * description).
 *
 * Like csilk_group_add_route() but enriches the route with metadata used by
 * the OpenAPI spec generator. The metadata is stored in the method handler
 * for later retrieval by csilk_generate_openapi_json().
 *
 * @param group       The route group.
 * @param method      HTTP method.
 * @param path        Path relative to the group prefix.
 * @param handler     The route handler function.
 * @param input_type  Registered reflection type name for the request body
 *                    (e.g., "CreateUserRequest"), or NULL.
 * @param output_type Registered reflection type name for the response body,
 *                    or NULL.
 * @param summary     Short description for OpenAPI operation summary.
 * @param description Detailed description for OpenAPI operation. */
void csilk_group_add_route_extended(csilk_group_t* group, const char* method,
                                    const char* path, csilk_handler_t handler,
                                    const char* input_type,
                                    const char* output_type,
                                    const char* summary,
                                    const char* description) {
  if (!group || !method || !path || !handler) return;

  char* full_path = join_path(group->prefix, path);
  if (!full_path) return;

  csilk_handler_t* combined_handlers = NULL;
  size_t combined_count = 0;

  if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
    free(full_path);
    free(combined_handlers);
    return;
  }

  csilk_handler_t* new_handlers = realloc(
      combined_handlers, (combined_count + 1) * sizeof(csilk_handler_t));
  if (!new_handlers) {
    free(full_path);
    free(combined_handlers);
    return;
  }
  combined_handlers = new_handlers;
  combined_handlers[combined_count] = handler;
  combined_count++;

  csilk_router_add_extended(group->router, method, full_path, combined_handlers,
                            combined_count, full_path, input_type, output_type,
                            summary, description);

  free(full_path);
  free(combined_handlers);
}

/** @copydoc csilk_group_add_route_extended
 *  @param perm_required  Permission required for this route, or NULL.
 *  @param perm_resource  Resource pattern for permission check, or NULL. */
void csilk_group_add_route_extended_perm(csilk_group_t* group,
                                         const char* method, const char* path,
                                         csilk_handler_t handler,
                                         const char* input_type,
                                         const char* output_type,
                                         const char* summary,
                                         const char* description,
                                         const char* perm_required,
                                         const char* perm_resource) {
  if (!group || !method || !path || !handler) return;

  char* full_path = join_path(group->prefix, path);
  if (!full_path) return;

  csilk_handler_t* combined_handlers = NULL;
  size_t combined_count = 0;

  if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
    free(full_path);
    free(combined_handlers);
    return;
  }

  csilk_handler_t* new_handlers = realloc(
      combined_handlers, (combined_count + 1) * sizeof(csilk_handler_t));
  if (!new_handlers) {
    free(full_path);
    free(combined_handlers);
    return;
  }
  combined_handlers = new_handlers;
  combined_handlers[combined_count] = handler;
  combined_count++;

  csilk_router_add_extended_perm(group->router, method, full_path,
                                 combined_handlers, combined_count, full_path,
                                 input_type, output_type, summary, description,
                                 perm_required, perm_resource);

  free(full_path);
  free(combined_handlers);
}

/** @brief Register a route with a custom chain of handlers (middleware + route
 * handler).
 *
 * Joins the group prefix with the route path, collects all group middleware
 * (from parent groups as well), appends the provided handlers, and registers
 * the combined chain with the router. The final handler in the chain should
 * typically call csilk_next() to pass control, and the last handler should
 * produce the response.
 *
 * @param group    The route group.
 * @param method   HTTP method.
 * @param path     Path relative to the group prefix.
 * @param handlers Array of handler functions (the chain).
 * @param count    Number of handlers in the array.
 * @note The handlers array is combined with group middleware — group
 *       middleware always runs first, followed by the provided handlers. */
void csilk_group_add_handlers(csilk_group_t* group, const char* method,
                              const char* path, csilk_handler_t* handlers,
                              size_t count) {
  if (!group || !handlers || count == 0) return;

  char* full_path = join_path(group->prefix, path);
  if (!full_path) return;

  csilk_handler_t* combined_handlers = NULL;
  size_t combined_count = 0;

  if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
    free(full_path);
    free(combined_handlers);
    return;
  }

  csilk_handler_t* new_handlers = realloc(
      combined_handlers, (combined_count + count) * sizeof(csilk_handler_t));
  if (!new_handlers) {
    free(full_path);
    free(combined_handlers);
    return;
  }
  combined_handlers = new_handlers;
  memcpy(combined_handlers + combined_count, handlers,
         count * sizeof(csilk_handler_t));
  combined_count += count;

  csilk_router_add(group->router, method, full_path, combined_handlers,
                   combined_count);

  free(full_path);
  free(combined_handlers);
}

/** @brief Free all resources associated with a route group.
 *
 * Releases the group's prefix string, middleware handlers array, and the
 * group struct itself. Does NOT free child groups or the router.
 *
 * @param group The group to free (may be NULL).
 * @note Child groups created with csilk_group_group() must be freed
 *       separately. The router is not owned by the group. */
void csilk_group_free(csilk_group_t* group) {
  if (!group) return;
  free(group->prefix);
  free(group->middlewares);
  free(group);
}
