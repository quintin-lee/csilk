/**
 * @file csilk_app.h
 * @brief High-level convenience API for the csilk framework.
 *
 * Provides a simplified "app" abstraction that wraps router, server,
 * config, logging, and middleware into a single easy-to-use interface.
 *
 * @code
 *   csilk_app_t* app = csilk_app_new("config.yaml");
 *   csilk_app_use(app, csilk_logger_handler);
 *   csilk_app_get(app, "/", hello_handler);
 *   csilk_app_get(app, "/user/:id", user_handler);
 *   csilk_app_post(app, "/login", login_handler);
 *   csilk_app_static(app, "/public", "./static");
 *   csilk_app_run(app, 8080);
 *   csilk_app_free(app);
 * @endcode
 *
 * @version 0.2.0
 * @copyright MIT License
 */

#ifndef CSILK_APP_H
#define CSILK_APP_H

#include "csilk.h"

/** @brief Opaque application handle. */
typedef struct csilk_app_s csilk_app_t;

/* ---- Lifecycle ---- */

/** @brief Create a new application with optional YAML config.
 * If config_path is NULL or the file doesn't exist, sensible defaults are used.
 * @param config_path Path to YAML config file, or NULL for defaults.
 * @return New application handle, or NULL on fatal error. */
csilk_app_t* csilk_app_new(const char* config_path);

/** @brief Deallocate all application resources.
 * @param app Application handle. */
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
 * @param app Application handle.
 * @param h Middleware handler function. */
void csilk_app_use(csilk_app_t* app, csilk_handler_t h);

/** @brief Register a middleware that runs only on a specific prefix group.
 * @param app Application handle.
 * @param prefix URL path prefix (e.g., "/api").
 * @param h Middleware handler function. */
void csilk_app_use_group(csilk_app_t* app, const char* prefix,
                         csilk_handler_t h);

/** @brief Auto-apply built-in middleware based on current config.
 * Enables: logger, recovery, CORS, CSRF, rate-limit, auth, gzip
 * according to the loaded YAML configuration.
 * @param app Application handle. */
void csilk_app_apply_config(csilk_app_t* app);

/* ---- Routes ---- */

/** @brief Register a route with a single handler.
 * @param app Application handle.
 * @param method HTTP method string (e.g., "GET").
 * @param path URL pattern (supports :param and *wildcard).
 * @param handler Route handler function. */
void csilk_app_add_route(csilk_app_t* app, const char* method, const char* path,
                         csilk_handler_t handler);

/** @brief Register a route with multiple handlers (middleware + handler).
 * @param app Application handle.
 * @param method HTTP method string.
 * @param path URL pattern.
 * @param handlers Array of handler functions.
 * @param n Number of handlers. */
void csilk_app_add_handlers(csilk_app_t* app, const char* method,
                            const char* path, csilk_handler_t* handlers,
                            size_t n);

/** @brief Register a route with OpenAPI metadata (input/output types).
 *  @param app Application handle.
 *  @param method HTTP method string.
 *  @param path URL pattern (supports :param and *wildcard).
 *  @param handler Route handler function.
 *  @param input_type Registered type name for request body (NULL if none).
 *  @param output_type Registered type name for response (NULL if none).
 *  @param summary Short operation summary (NULL if none).
 *  @param description Detailed operation description (NULL if none). */
void csilk_app_add_route_extended(csilk_app_t* app, const char* method,
                                  const char* path, csilk_handler_t handler,
                                  const char* input_type,
                                  const char* output_type, const char* summary,
                                  const char* description);

/** @brief Convenience macro to register a GET route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_get(app, path, handler) \
  csilk_app_add_route(app, "GET", path, handler)
/** @brief Convenience macro to register a GET route with OpenAPI metadata. */
#define csilk_app_get_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "GET", path, handler, in, out, summary, \
                               desc)
/** @brief Convenience macro to register a POST route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_post(app, path, handler) \
  csilk_app_add_route(app, "POST", path, handler)
/** @brief Convenience macro to register a POST route with OpenAPI metadata. */
#define csilk_app_post_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "POST", path, handler, in, out, summary, \
                               desc)
/** @brief Convenience macro to register a PUT route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_put(app, path, handler) \
  csilk_app_add_route(app, "PUT", path, handler)
/** @brief Convenience macro to register a PUT route with OpenAPI metadata. */
#define csilk_app_put_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "PUT", path, handler, in, out, summary, \
                               desc)
/** @brief Convenience macro to register a DELETE route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_delete(app, path, handler) \
  csilk_app_add_route(app, "DELETE", path, handler)
/** @brief Convenience macro to register a DELETE route with OpenAPI metadata.
 */
#define csilk_app_delete_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "DELETE", path, handler, in, out, summary, \
                               desc)
/** @brief Convenience macro to register a PATCH route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_patch(app, path, handler) \
  csilk_app_add_route(app, "PATCH", path, handler)
/** @brief Convenience macro to register a PATCH route with OpenAPI metadata. */
#define csilk_app_patch_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "PATCH", path, handler, in, out, summary, \
                               desc)
/** @brief Convenience macro to register an OPTIONS route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_options(app, path, handler) \
  csilk_app_add_route(app, "OPTIONS", path, handler)
/** @brief Convenience macro to register an OPTIONS route with OpenAPI metadata.
 */
#define csilk_app_options_ext(app, path, handler, in, out, summary, desc) \
  csilk_app_add_route_extended(app, "OPTIONS", path, handler, in, out,    \
                               summary, desc)
/** @brief Convenience macro to register a HEAD route via the app API.
 *  @param app Application handle.
 *  @param path URL path pattern.
 *  @param handler Handler function. */
#define csilk_app_head(app, path, handler) \
  csilk_app_add_route(app, "HEAD", path, handler)
/** @brief Convenience macro to register a HEAD route with OpenAPI metadata. */
#define csilk_app_head_ext(app, path, handler, in, out, summary, desc)       \
  csilk_app_add_route_extended(app, "HEAD", path, handler, in, out, summary, \
                               desc)

/* ---- Static Files ---- */

/** @brief Serve static files from a local directory.
 * @param app Application handle.
 * @param prefix URL path prefix (e.g., "/static").
 * @param root_dir Local directory to serve files from. */
void csilk_app_static(csilk_app_t* app, const char* prefix,
                      const char* root_dir);

/* ---- Configuration ---- */

/** @brief Apply server-level configuration (timeouts, limits, TCP options).
 * @param app Application handle.
 * @param c Server configuration struct. */
void csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c);

/** @brief Get a copy of the current application configuration.
 * @param app Application handle.
 * @return Copy of internal config (caller must csilk_config_free it),
 *         or NULL on error. */
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
