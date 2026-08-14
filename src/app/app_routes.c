/**
 * @file app_routes.c
 * @brief Route registration and group management for csilk_app_t.
 *
 * Extracted from app.c to keep that file focused on lifecycle and config.
 *
 * @copyright MIT License
 */

#include "csilk/app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "app_internal.h"

/** @brief Find an existing group by prefix, or create a new one.
 *
 * ## Lookup strategy
 * 1. Root prefix ("", "/") — return root_group (lazy-created on first call).
 * 2. Linear scan groups[] cache — O(n) with n capped at CSILK_MAX_GROUPS (32).
 * 3. Cache miss — create a child group under root_group via
 *    csilk_group_group(), store in cache, return.
 *
 * @param app Application handle.
 * @param prefix URL path prefix.
 * @return Route group instance, or NULL on failure. */
csilk_group_t*
find_or_create_group(csilk_app_t* app, const char* prefix)
{
    if (!prefix || !*prefix || !strcmp(prefix, "/")) {
        if (!app->root_group) {
            app->root_group = csilk_group_new(app->router, "");
            CSILK_LOG_D("Created root route group");
        }
        return app->root_group;
    }
    for (int i = 0; i < app->group_count; i++) {
        if (!strcmp(app->groups[i].prefix, prefix)) {
            CSILK_LOG_T("Found existing group for prefix: %s", prefix);
            return app->groups[i].group;
        }
    }

    if (app->group_count >= CSILK_MAX_GROUPS) {
        CSILK_LOG_E("Failed to create route group: max group limit (%d) reached", CSILK_MAX_GROUPS);
        return NULL;
    }

    if (!app->root_group) {
        app->root_group = csilk_group_new(app->router, "");
        CSILK_LOG_D("Created root route group");
    }

    csilk_group_t* g = app->root_group ? csilk_group_group(app->root_group, prefix)
                                       : csilk_group_new(app->router, prefix);
    if (!g) {
        CSILK_LOG_E("Failed to create subgroup for prefix: %s", prefix);
        return NULL;
    }

    int n = app->group_count++;
    snprintf(app->groups[n].prefix, sizeof(app->groups[n].prefix), "%s", prefix);
    app->groups[n].group = g;
    CSILK_LOG_I("Created route group prefix: %s", prefix);
    return g;
}

/** @brief Internal: select the registered group whose prefix longest-matches
 * the request path, and compute the relative path within it. */
static csilk_group_t*
find_matching_group_for_path(csilk_app_t* app, const char* path, const char** out_relative_path)
{
    csilk_group_t* best_group = app->root_group;
    size_t         best_len = 0;
    *out_relative_path = path;

    CSILK_LOG_T("Matching path '%s' against %d registered groups", path, app->group_count);

    for (int i = 0; i < app->group_count; i++) {
        const char* prefix = app->groups[i].prefix;
        size_t      prefix_len = strlen(prefix);
        if (prefix_len > best_len && strncmp(path, prefix, prefix_len) == 0) {
            if (path[prefix_len] == '\0' || path[prefix_len] == '/') {
                best_group = app->groups[i].group;
                best_len = prefix_len;
                *out_relative_path = path + prefix_len;
            }
        }
    }
    CSILK_LOG_D("Matched best group with prefix length %zu, relative path: '%s'",
                best_len,
                *out_relative_path);
    return best_group;
}

/* ---- middleware ---- */

/** @brief Register a middleware handler scoped to a specific URL prefix group.
 *
 * Creates (or finds) a route group for the given prefix and adds the
 * middleware to it. The middleware runs for any route whose path starts
 * with the given prefix.
 *
 * @param app    Application instance.
 * @param prefix URL prefix (e.g., "/api/admin").
 * @param h      Middleware handler function.
 * @return void
 */
void
csilk_app_use_group(csilk_app_t* app, const char* prefix, csilk_handler_t h)
{
    if (!app || !prefix) {
        return;
    }
    csilk_group_t* g = find_or_create_group(app, prefix);
    if (g) {
        csilk_group_use(g, h);
        CSILK_LOG_I("Registered group middleware for prefix '%s': %p", prefix, (void*)h);
    }
}

/** @brief Apply configuration-driven middleware settings.
 *
 * Reads the current app config and sets up static file serving if
 * config.static_files.enable is true and root_dir is configured. The
 * prefix defaults to "/static" if not specified in the config.
 *
 * @param app Application instance.
 * @return void
 */
void
csilk_app_apply_config(csilk_app_t* app)
{
    if (!app) {
        return;
    }
    if (app->config.static_files.enable && app->config.static_files.root_dir) {
        csilk_app_static(app,
                         app->config.static_files.prefix ? app->config.static_files.prefix
                                                         : "/static",
                         app->config.static_files.root_dir);
    }
}

/* ---- routes ---- */

/** @brief Register a route on the root group with a single handler.
 *
 * @param app    Application instance.
 * @param method HTTP method (e.g., "GET", "POST").
 * @param path   URL path (e.g., "/users").
 * @param handler Handler function.
 * @return void
 */
void
csilk_app_add_route(csilk_app_t* app, const char* method, const char* path, csilk_handler_t handler)
{
    if (!app || !method || !path || !handler) {
        return;
    }
    const char*    relative_path = NULL;
    csilk_group_t* g = find_matching_group_for_path(app, path, &relative_path);
    if (!g) {
        CSILK_LOG_E("Failed to add route %s %s: group match failed", method, path);
        return;
    }
    csilk_group_add_route(g, method, relative_path, handler);
    CSILK_LOG_I("Route registered: %s %s", method, path);
}

/** @brief Register a route on the root group with a single handler and OpenAPI
 * metadata.
 *
 * @param app         Application instance.
 * @param method      HTTP method.
 * @param path        URL path.
 * @param handler     Handler function.
 * @param input_type  Registered type name for request body JSON schema.
 * @param output_type Registered type name for response body JSON schema.
 * @param summary     Short description for the OpenAPI operation.
 * @param description Detailed description for the OpenAPI operation.
 * @return void
 */
void
csilk_app_add_route_extended(csilk_app_t*    app,
                             const char*     method,
                             const char*     path,
                             csilk_handler_t handler,
                             const char*     input_type,
                             const char*     output_type,
                             const char*     summary,
                             const char*     description)
{
    if (!app || !method || !path || !handler) {
        return;
    }
    const char*    relative_path = NULL;
    csilk_group_t* g = find_matching_group_for_path(app, path, &relative_path);
    if (!g) {
        CSILK_LOG_E("Failed to add route %s %s: group match failed", method, path);
        return;
    }
    csilk_group_add_route_extended(
        g, method, relative_path, handler, input_type, output_type, summary, description);
    CSILK_LOG_I("Route registered (with OpenAPI metadata): %s %s", method, path);
}

/** @copydoc csilk_app_add_route_extended
 *  @param perm_required  Permission required for this route, or NULL.
 *  @param perm_resource  Resource pattern for permission check, or NULL. */
void
csilk_app_add_route_extended_perm(csilk_app_t*    app,
                                  const char*     method,
                                  const char*     path,
                                  csilk_handler_t handler,
                                  const char*     input_type,
                                  const char*     output_type,
                                  const char*     summary,
                                  const char*     description,
                                  const char*     perm_required,
                                  const char*     perm_resource)
{
    if (!app || !method || !path || !handler) {
        return;
    }
    const char*    relative_path = NULL;
    csilk_group_t* g = find_matching_group_for_path(app, path, &relative_path);
    if (!g) {
        CSILK_LOG_E("Failed to add route %s %s: group match failed", method, path);
        return;
    }
    csilk_group_add_route_extended_perm(g,
                                        method,
                                        relative_path,
                                        handler,
                                        input_type,
                                        output_type,
                                        summary,
                                        description,
                                        perm_required,
                                        perm_resource);
    CSILK_LOG_I("Route registered (with Perm/OpenAPI metadata): %s %s (perm: %s on %s)",
                method,
                path,
                perm_required ? perm_required : "none",
                perm_resource ? perm_resource : "none");
}

/** @brief Register a route with permission metadata.
 *  @param app            Application instance.
 *  @param method         HTTP method.
 *  @param path           URL path.
 *  @param handler        Handler function.
 *  @param perm_required  Permission identifier (e.g., "read"), or NULL.
 * @param perm_resource  Resource pattern (e.g., "users:*"), or NULL.
 * @return void
 */
void
csilk_app_add_route_perm(csilk_app_t*    app,
                         const char*     method,
                         const char*     path,
                         csilk_handler_t handler,
                         const char*     perm_required,
                         const char*     perm_resource)
{
    csilk_app_add_route_extended_perm(
        app, method, path, handler, NULL, NULL, NULL, NULL, perm_required, perm_resource);
}

/** @brief Register a route with a custom handler chain on the root group.
 *
 * @param app      Application instance.
 * @param method   HTTP method.
 * @param path     URL path.
 * @param handlers Array of handler functions.
 * @param n        Number of handlers in the array.
 * @return void
 */
void
csilk_app_add_handlers(
    csilk_app_t* app, const char* method, const char* path, csilk_handler_t* handlers, size_t n)
{
    if (!app || !method || !path || !handlers || n == 0) {
        return;
    }
    const char*    relative_path = NULL;
    csilk_group_t* g = find_matching_group_for_path(app, path, &relative_path);
    if (!g) {
        CSILK_LOG_E("Failed to add handler chain %s %s: group match failed", method, path);
        return;
    }
    csilk_group_add_handlers(g, method, relative_path, handlers, n);
    CSILK_LOG_I("Route chain registered: %s %s (handlers count: %zu)", method, path, n);
}
