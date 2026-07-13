#pragma once
/**
 * @file group.h
 * @brief Route group abstraction for prefix-scoped route registration.
 *
 * Groups allow sharing a common prefix and middleware set across multiple
 * routes (e.g., "/api/v1").  Supports nesting via csilk_group_group.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#include "csilk/core/router.h"

/**
 * @brief Create a new route group with a URL prefix.
 *
 * Groups allow sharing a common prefix and middleware set across multiple
 * routes (e.g., "/api/v1").
 *
 * @param router The router to attach the group to.
 * @param prefix URL prefix for all routes in this group (e.g., "/api/v1").
 * @return A new csilk_group_t instance, or nullptr on allocation failure.
 */
csilk_group_t* csilk_group_new(csilk_router_t* router, const char* prefix);

/**
 * @brief Create a nested sub-group within an existing group.
 *
 * The sub-group inherits the parent's middleware and its prefix is
 * concatenated.
 *
 * @param parent The parent group.
 * @param prefix Sub-prefix appended to the parent's prefix (e.g., "admin").
 * @return A new sub-group instance, or nullptr on allocation failure.
 */
csilk_group_t* csilk_group_group(csilk_group_t* parent, const char* prefix);

/**
 * @brief Add middleware to a group.
 *
 * Middleware is stored in the order it is added and is executed for every
 * route in the group (and any nested sub-groups).
 *
 * @param group   The route group.
 * @param handler Middleware function to prepend to all group routes.
 * @return 0 on success, -1 on failure (e.g., realloc failure).
 */
int csilk_group_use(csilk_group_t* group, csilk_handler_t handler);

/**
 * @brief Add a route to the group.
 *
 * The full URL pattern is the group prefix concatenated with @p path.
 * The group's middleware is prepended to the handler.
 *
 * @param group   The route group.
 * @param method  HTTP method.
 * @param path    Path relative to the group prefix (e.g., "/:id").
 * @param handler The route handler function.
 */
int csilk_group_add_route(csilk_group_t*  group,
                          const char*     method,
                          const char*     path,
                          csilk_handler_t handler);

/**
 * @brief Add a route with OpenAPI/reflection metadata to a group.
 *
 * Extended version that also records input/output types and documentation
 * for automatic OpenAPI spec generation.
 *
 * @param group       The route group.
 * @param method      HTTP method.
 * @param path        Path relative to the group prefix.
 * @param handler     The route handler function.
 * @param input_type  Registered type name for request-body binding (nullptr if
 * none).
 * @param output_type Registered type name for response serialisation (nullptr if
 * none).
 * @param summary     Short operation summary for OpenAPI (nullptr to omit).
 * @param description Detailed operation description for OpenAPI (nullptr to omit).
 */
int csilk_group_add_route_extended(csilk_group_t*  group,
                                   const char*     method,
                                   const char*     path,
                                   csilk_handler_t handler,
                                   const char*     input_type,
                                   const char*     output_type,
                                   const char*     summary,
                                   const char*     description);

int csilk_group_add_route_extended_perm(csilk_group_t*  group,
                                        const char*     method,
                                        const char*     path,
                                        csilk_handler_t handler,
                                        const char*     input_type,
                                        const char*     output_type,
                                        const char*     summary,
                                        const char*     description,
                                        const char*     perm_required,
                                        const char*     perm_resource);

/**
 * @brief Add a route with an explicit array of handlers.
 *
 * Useful when you need to attach multiple middleware + the final handler
 * without calling group_use first.
 *
 * @param group    The route group.
 * @param method   HTTP method.
 * @param path     Path relative to the group prefix.
 * @param handlers Array of handler function pointers (middleware first,
 *                 route handler last).  Stored by pointer — must outlive
 *                 the router.
 * @param count    Number of elements in @p handlers.
 */
int csilk_group_add_handlers(csilk_group_t*   group,
                             const char*      method,
                             const char*      path,
                             csilk_handler_t* handlers,
                             size_t           count);

/**
 * @brief Destroy a route group and release its resources.
 *
 * Frees the group struct and its prefix string.  Does NOT free the
 * associated router or any handler functions.
 *
 * @param group The group to free.  Must not be nullptr.
 */
void csilk_group_free(csilk_group_t* group);

/** @name Group Route Macros
 *  Convenience macros for adding routes to groups.
 *  @{ */
/** @brief Register a GET route on the group. */
#define csilk_GET(group, path, handler)                                                            \
    do {                                                                                           \
        csilk_group_add_route(group, "GET", path, handler);                                        \
    } while (0)
/** @brief Register a POST route on the group. */
#define csilk_POST(group, path, handler)                                                           \
    do {                                                                                           \
        csilk_group_add_route(group, "POST", path, handler);                                       \
    } while (0)
/** @brief Register a PUT route on the group. */
#define csilk_PUT(group, path, handler)                                                            \
    do {                                                                                           \
        csilk_group_add_route(group, "PUT", path, handler);                                        \
    } while (0)
/** @brief Register a DELETE route on the group. */
#define csilk_DELETE(group, path, handler)                                                         \
    do {                                                                                           \
        csilk_group_add_route(group, "DELETE", path, handler);                                     \
    } while (0)
/** @brief Register a PATCH route on the group. */
#define csilk_PATCH(group, path, handler)                                                          \
    do {                                                                                           \
        csilk_group_add_route(group, "PATCH", path, handler);                                      \
    } while (0)
/** @brief Register an OPTIONS route on the group. */
#define csilk_OPTIONS(group, path, handler)                                                        \
    do {                                                                                           \
        csilk_group_add_route(group, "OPTIONS", path, handler);                                    \
    } while (0)
/** @brief Register a HEAD route on the group. */
#define csilk_HEAD(group, path, handler)                                                           \
    do {                                                                                           \
        csilk_group_add_route(group, "HEAD", path, handler);                                       \
    } while (0)
/** @} */
