/**
 * @file csilk_app.h
 * @brief High-level convenience API for the csilk framework.
 *
 * Provides a simplified "app" abstraction that wraps router, server,
 * config, logging, and middleware into a single easy-to-use interface.
 * The app follows the same patterns as Gin (Golang): a central app object
 * owns the router, server, and config; middleware is registered globally
 * or per-group via csilk_app_use; and routes are added with method-specific
 * macros (csilk_app_get, csilk_app_post, etc.).
 *
 * ## Lifecycle
 *   1. csilk_app_new(config_path) — creates the app, loads config, initializes
 *      subsystems (DB, AI, logger).
 *   2. csilk_app_use / csilk_app_get / csilk_app_post — register middleware and
 *      routes.
 *   3. csilk_app_run — starts the I/O event loop (libuv or io_uring, blocks).
 *   4. csilk_app_free — cleans up all resources.
 *
 * ## Thread Safety
 *   All app-level functions must be called from the main thread during setup
 *   (before csilk_app_run).  The server itself runs single-threaded on the I/O event loop.
 *
 * @example
 *   csilk_app_t* app = csilk_app_new("config.yaml");
 *   csilk_app_use(app, csilk_logger_handler);
 *   csilk_app_get(app, "/", hello_handler);
 *   csilk_app_get(app, "/user/:id", user_handler);
 *   csilk_app_post(app, "/login", login_handler);
 *   csilk_app_static(app, "/public", "./static");
 *   csilk_app_run(app, 8080);
 *   csilk_app_free(app);
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_APP_H
#define CSILK_APP_H

/** @brief Opaque application handle. */
typedef struct csilk_app_s csilk_app_t;

#include "csilk/core/types.h"
#include "csilk/core/config.h"

/* ---- Lifecycle ---- */

/** @brief Create a new application with optional YAML config.
 *
 * Initialises the router, server, logger, database subsystem, and AI
 * subsystem.  If @p config_path is provided and readable, settings for
 * CORS, rate-limiting, static files, auth, AI, and cipher are loaded.
 * If config_path is nullptr or the file doesn't exist, sensible defaults
 * are used (port 8080, stderr logging, all middleware disabled).
 *
 * @param config_path Path to YAML config file, or nullptr for defaults.
 * @return New application handle, or nullptr on fatal error. */
csilk_app_t* csilk_app_new(const char* config_path);

/** @brief Deallocate all application resources.
 *
 * Stops the server (if running), frees the router, config, and any
 * registered middleware.  Safe to call with a nullptr @p app.
 *
 * @param app Application handle (may be nullptr). */
void csilk_app_free(csilk_app_t* app);

/* ---- Logger ---- */

/** @brief Set the minimum log level.
 * @param app Application handle.
 * @param level Minimum log level. */
void csilk_app_log_level(csilk_app_t* app, csilk_log_level_t level);

/** @brief Enable file logging with optional rotation.
 * @param app Application handle.
 * @param path Log file path.
 * @param max_sz Max file size before rotation (0 to disable). */
void csilk_app_log_file(csilk_app_t* app, const char* path, size_t max_sz);

/** @brief Enable or disable JSON structured log output.
 * @param app Application handle.
 * @param enable 1 to enable JSON format, 0 for plain text. */
void csilk_app_log_json(csilk_app_t* app, int enable);

/* ---- Middleware ---- */

/** @brief Register a global middleware that runs on every route.
 *
 * Middleware is executed in registration order for every request, before
 * the route-specific handler.  The onion model applies — code before
 * csilk_next() runs on the way in, code after runs on the way out.
 *
 * @param app Application handle.
 * @param h Middleware handler function. */
void csilk_app_use(csilk_app_t* app, csilk_handler_t h);

/** @brief Register a middleware that runs only on a specific prefix group.
 *
 * Group middleware is prepended to routes whose path matches @p prefix.
 * For example, a middleware registered with prefix "/api" runs on all
 * "/api/..." routes but not on "/health".
 *
 * @param app Application handle.
 * @param prefix URL path prefix (e.g., "/api").
 * @param h Middleware handler function. */
void csilk_app_use_group(csilk_app_t* app, const char* prefix, csilk_handler_t h);

/** @brief Auto-apply built-in middleware based on current config.
 *
 * Reads the loaded YAML configuration and installs the following
 * middleware when the corresponding config flags are enabled:
 * logger, recovery, CORS, CSRF, rate-limit, auth, gzip.
 * Safe to call even without a loaded config — disabled options are no-ops.
 *
 * @param app Application handle. */
void csilk_app_apply_config(csilk_app_t* app);

/* ---- Routes ---- */

/** @brief Register a route with a single handler.
 * @param app Application handle.
 * @param method HTTP method string (e.g., "GET").
 * @param path URL pattern (supports :param and *wildcard).
 * @param handler Route handler function. */
void csilk_app_add_route(csilk_app_t*    app,
                         const char*     method,
                         const char*     path,
                         csilk_handler_t handler);

/** @brief Register a route with multiple handlers (middleware + handler).
 * @param app Application handle.
 * @param method HTTP method string.
 * @param path URL pattern.
 * @param handlers Array of handler functions.
 * @param n Number of handlers. */
void csilk_app_add_handlers(
    csilk_app_t* app, const char* method, const char* path, csilk_handler_t* handlers, size_t n);

/** @brief Register a route with OpenAPI metadata (input/output types).
 *  @param app Application handle.
 *  @param method HTTP method string.
 *  @param path URL pattern (supports :param and *wildcard).
 *  @param handler Route handler function.
 *  @param input_type Registered type name for request body (nullptr if none).
 *  @param output_type Registered type name for response (nullptr if none).
 *  @param summary Short operation summary (nullptr if none).
 *  @param description Detailed operation description (nullptr if none). */
void csilk_app_add_route_extended(csilk_app_t*    app,
                                  const char*     method,
                                  const char*     path,
                                  csilk_handler_t handler,
                                  const char*     input_type,
                                  const char*     output_type,
                                  const char*     summary,
                                  const char*     description);

/** @brief Register a route with permission metadata.
 *  @param app Application handle.
 *  @param method HTTP method string.
 *  @param path URL pattern (supports :param and *wildcard).
 *  @param handler Route handler function.
 *  @param perm_required Permission identifier (e.g., "read"), or nullptr.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
void csilk_app_add_route_perm(csilk_app_t*    app,
                              const char*     method,
                              const char*     path,
                              csilk_handler_t handler,
                              const char*     perm_required,
                              const char*     perm_resource);

/** @brief Register a route with full metadata including permissions.
 *  @param app Application handle.
 *  @param method HTTP method string.
 *  @param path URL pattern.
 *  @param handler Route handler function.
 *  @param input_type Registered type name for request body (nullptr if none).
 *  @param output_type Registered type name for response (nullptr if none).
 *  @param summary Short operation summary (nullptr if none).
 *  @param description Detailed operation description (nullptr if none).
 *  @param perm_required Permission identifier (e.g., "read"), or nullptr.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
void csilk_app_add_route_extended_perm(csilk_app_t*    app,
                                       const char*     method,
                                       const char*     path,
                                       csilk_handler_t handler,
                                       const char*     input_type,
                                       const char*     output_type,
                                       const char*     summary,
                                       const char*     description,
                                       const char*     perm_required,
                                       const char*     perm_resource);

/** @brief Convenience macro to register a GET route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_get(app, path, handler)                                                          \
    do {                                                                                           \
        csilk_app_add_route(app, "GET", path, handler);                                            \
    } while (0)
/** @brief Convenience macro to register a GET route with OpenAPI metadata. */
#define csilk_app_get_ext(app, path, handler, in, out, summary, desc)                              \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "GET", path, handler, in, out, summary, desc);           \
    } while (0)
/** @brief Convenience macro to register a GET route with permission. */
#define csilk_app_get_perm(app, path, handler, perm, res)                                          \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "GET", path, handler, perm, res);                            \
    } while (0)
/** @brief Convenience macro to register a POST route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_post(app, path, handler)                                                         \
    do {                                                                                           \
        csilk_app_add_route(app, "POST", path, handler);                                           \
    } while (0)
/** @brief Convenience macro to register a POST route with OpenAPI metadata. */
#define csilk_app_post_ext(app, path, handler, in, out, summary, desc)                             \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "POST", path, handler, in, out, summary, desc);          \
    } while (0)
/** @brief Convenience macro to register a POST route with permission. */
#define csilk_app_post_perm(app, path, handler, perm, res)                                         \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "POST", path, handler, perm, res);                           \
    } while (0)
/** @brief Convenience macro to register a PUT route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_put(app, path, handler)                                                          \
    do {                                                                                           \
        csilk_app_add_route(app, "PUT", path, handler);                                            \
    } while (0)
/** @brief Convenience macro to register a PUT route with OpenAPI metadata. */
#define csilk_app_put_ext(app, path, handler, in, out, summary, desc)                              \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "PUT", path, handler, in, out, summary, desc);           \
    } while (0)
/** @brief Convenience macro to register a PUT route with permission. */
#define csilk_app_put_perm(app, path, handler, perm, res)                                          \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "PUT", path, handler, perm, res);                            \
    } while (0)
/** @brief Convenience macro to register a DELETE route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_delete(app, path, handler)                                                       \
    do {                                                                                           \
        csilk_app_add_route(app, "DELETE", path, handler);                                         \
    } while (0)
/** @brief Convenience macro to register a DELETE route with OpenAPI metadata.
 */
#define csilk_app_delete_ext(app, path, handler, in, out, summary, desc)                           \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "DELETE", path, handler, in, out, summary, desc);        \
    } while (0)
/** @brief Convenience macro to register a DELETE route with permission. */
#define csilk_app_delete_perm(app, path, handler, perm, res)                                       \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "DELETE", path, handler, perm, res);                         \
    } while (0)
/** @brief Convenience macro to register a PATCH route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_patch(app, path, handler)                                                        \
    do {                                                                                           \
        csilk_app_add_route(app, "PATCH", path, handler);                                          \
    } while (0)
/** @brief Convenience macro to register a PATCH route with OpenAPI metadata. */
#define csilk_app_patch_ext(app, path, handler, in, out, summary, desc)                            \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "PATCH", path, handler, in, out, summary, desc);         \
    } while (0)
/** @brief Convenience macro to register a PATCH route with permission. */
#define csilk_app_patch_perm(app, path, handler, perm, res)                                        \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "PATCH", path, handler, perm, res);                          \
    } while (0)
/** @brief Convenience macro to register an OPTIONS route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_options(app, path, handler)                                                      \
    do {                                                                                           \
        csilk_app_add_route(app, "OPTIONS", path, handler);                                        \
    } while (0)
/** @brief Convenience macro to register an OPTIONS route with OpenAPI metadata.
 */
#define csilk_app_options_ext(app, path, handler, in, out, summary, desc)                          \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "OPTIONS", path, handler, in, out, summary, desc);       \
    } while (0)
/** @brief Convenience macro to register an OPTIONS route with permission. */
#define csilk_app_options_perm(app, path, handler, perm, res)                                      \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "OPTIONS", path, handler, perm, res);                        \
    } while (0)
/** @brief Convenience macro to register a HEAD route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_head(app, path, handler)                                                         \
    do {                                                                                           \
        csilk_app_add_route(app, "HEAD", path, handler);                                           \
    } while (0)
/** @brief Convenience macro to register a HEAD route with OpenAPI metadata. */
#define csilk_app_head_ext(app, path, handler, in, out, summary, desc)                             \
    do {                                                                                           \
        csilk_app_add_route_extended(app, "HEAD", path, handler, in, out, summary, desc);          \
    } while (0)
/** @brief Convenience macro to register a HEAD route with permission. */
#define csilk_app_head_perm(app, path, handler, perm, res)                                         \
    do {                                                                                           \
        csilk_app_add_route_perm(app, "HEAD", path, handler, perm, res);                           \
    } while (0)

/* ---- Static Files ---- */

/** @brief Serve static files from a local directory.
 * @param app Application handle.
 * @param prefix URL path prefix (e.g., "/static").
 * @param root_dir Local directory to serve files from. */
void csilk_app_static(csilk_app_t* app, const char* prefix, const char* root_dir);

/* ---- Configuration ---- */

/** @brief Apply server-level configuration (timeouts, limits, TCP options).
 * @param app Application handle.
 * @param c Server configuration struct. */
void csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c);

/** @brief Get a copy of the current application configuration.
 * @param app Application handle.
 * @return Copy of internal config (caller must csilk_config_free it),
 *         or nullptr on error. */
csilk_config_t* csilk_app_config(csilk_app_t* app);

/* ---- OpenAPI / Swagger ---- */

/** @brief Enable or disable the built-in /openapi.json endpoint.
 *  The endpoint is enabled by default when the app is created.
 * @param app Application handle.
 * @param enable 1 to enable, 0 to disable (handler returns 404). */
void csilk_app_enable_openapi(csilk_app_t* app, int enable);

/* ---- Run ---- */

/** @brief Start the server and block until stopped (Ctrl+C).
 * @param app Application handle.
 * @param port TCP port to listen on.
 * @return 0 on clean exit, -1 on error. */
int csilk_app_run(csilk_app_t* app, int port);

/* ---- Access underlying objects (advanced use) ---- */

/** @brief Get the underlying router for advanced operations.
 * @param app Application handle.
 * @return Router handle. */
csilk_router_t* csilk_app_router(csilk_app_t* app);

/** @brief Get the underlying server for advanced operations.
 * @param app Application handle.
 * @return Server handle. */
csilk_server_t* csilk_app_server(csilk_app_t* app);

#endif /* CSILK_APP_H */
