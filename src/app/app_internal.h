/**
 * @file app_internal.h
 * @brief Internal definition of struct csilk_app_s for cross-file use.
 *
 * Shared between app.c and app_routes.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_APP_INTERNAL_H
#define CSILK_APP_INTERNAL_H

#include "csilk/csilk.h"

/** @brief Maximum number of route groups per app. */
enum { CSILK_MAX_GROUPS = 32 };

/** @brief Maximum number of dual-parameter middleware thunks per app (csilk#9c). */
enum { CSILK_MAX_MW_THUNKS = 32 };

/** @brief Initial capacity for group middleware array. */
enum { CSILK_GROUP_MW_INIT_CAP = 4 };

/** @brief Route group — holds a URL prefix, middleware chain, and optional
 * parent group for nesting.
 *
 * Groups form a tree via the parent pointer. The full middleware chain
 * for any route is the concatenation of parent middleware (recursively)
 * followed by this group's middleware, followed by the route handlers.
 */
struct csilk_group_s {
    char*            prefix;
    csilk_router_t*  router;
    csilk_handler_t* middlewares;
    size_t           middleware_count;
    size_t           middleware_capacity;
    csilk_group_t*   parent;
    int              has_routes;  /**< Set when any route registered (csilk#9e). */
    int              exact_match; /**< Exact prefix match only (csilk#9e). */
};

/** @brief Internal: thunk capturing a csilk_middleware_t + user_data pair so a
 * dual-parameter middleware can be registered as a csilk_handler_t (csilk#9c). */
typedef struct {
    csilk_middleware_t fn;
    void*              user_data;
} mw_thunk_t;

/** @brief Internal: cached route group lookup entry for fast prefix-to-group
 * mapping. */
typedef struct {
    char           prefix[128];
    csilk_group_t* group;
} cached_group_t;

/** @brief Main application structure containing config, router, server, and
 * groups.
 *
 * Lifecycle: created in csilk_app_new(), destroyed in csilk_app_free().
 * Ownership: owns everything except the OpenAPI router pointer (global). */
struct csilk_app_s {
    csilk_config_t  config;
    csilk_router_t* router;
    csilk_server_t* server;
    csilk_group_t*  root_group;
    cached_group_t  groups[CSILK_MAX_GROUPS];
    int             group_count;
};

csilk_group_t* find_or_create_group(csilk_app_t* app, const char* prefix);

#endif
