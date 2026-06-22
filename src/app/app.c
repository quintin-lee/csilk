/**
 * @file app.c
 * @brief High-level convenience API — csilk_app_t implementation.
 *
 * ## Architecture
 * csilk_app_t is the top-level facade. It owns one router, one server, one
 * root route group, and a config struct. Users interact only through the app
 * handle; internal wiring (router -> server, group -> router) is hidden.
 *
 * ## Bootstrap Sequence
 * csilk_app_new() runs in this order:
 *   1. Load YAML config (or apply hard-coded defaults).
 *   2. Initialize the logger from config.
 *   3. Create router + server, wire them together.
 *   4. Register built-in middleware (recovery, request logging) on the server.
 *   5. Create the root route group.
 *   6. Register built-in endpoints: /openapi.json, /docs, /csilk-docs/.
 *
 * ## Routing & Static Files
 * Routes are added via the root group (csilk_group_t), which assembles a
 * combined middleware + handler chain and hands it to the router. Static
 * file serving uses a separate global table mapping URL prefixes to local
 * directory roots, dispatched by a single static_serve handler.
 *
 * ## OpenAPI / Swagger
 * The OpenAPI router reference is guarded by a process-level mutex so it can
 * be toggled on/off safely at runtime. Two built-in handlers serve the spec
 * JSON and the Swagger UI HTML page.
 *
 * @copyright MIT License
 * @version 0.2.1
 */

#include "csilk/app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/srv_internal.h"

/** @brief Internal: cached route group lookup entry for fast prefix-to-group
 * mapping.
 *
 * Avoids redundant group creation when the same prefix is referenced
 * multiple times (e.g., csilk_app_use_group then csilk_app_add_route).
 * The cache is linear-scanned; CSILK_MAX_GROUPS (32) keeps it cheap. */
typedef struct {
	char prefix[128];     /**< URL path prefix — the lookup key. */
	csilk_group_t* group; /**< Cached group handle — created lazily by
                         *   find_or_create_group(). Freed once in
                         *   csilk_app_free(). */
} cached_group_t;

/** @brief Internal: descriptor for a static file serving route mapping URL
 * prefix to filesystem directory.
 *
 * Stored in a fixed-size global table (g_static[]). The static_serve
 * handler scans the table on each request to find the matching root_dir
 * for the requested path. */
typedef struct {
	char url_prefix[128]; /**< URL path prefix for static files (e.g., "/static").
                         *   Used as the lookup key in static_serve(). */
	char root_dir[256];   /**< Local filesystem directory path served for this
                         *   prefix. Passed to csilk_static() at dispatch time. */
} static_route_t;

/** @brief Router reference for the built-in OpenAPI handler. */
static csilk_router_t* s_openapi_router = nullptr;
static uv_mutex_t s_app_mutex;
static uv_once_t s_app_mutex_once = UV_ONCE_INIT;

/** @brief Internal: initialize the application-level mutex (called once via
 * uv_once).
 *
 * Creates the mutex that protects the shared OpenAPI router reference and
 * the static file route table. */
static void
init_app_mutex(void)
{
	uv_mutex_init(&s_app_mutex);
}

/** @brief Internal: safely retrieve the current OpenAPI router under the app
 * mutex.
 *
 * @return The current s_openapi_router pointer, or nullptr.
 * @note Thread-safe. The returned pointer should not be stored beyond the
 *       calling scope as it may change. */
static csilk_router_t*
get_openapi_router(void)
{
	uv_mutex_lock(&s_app_mutex);
	csilk_router_t* r = s_openapi_router;
	uv_mutex_unlock(&s_app_mutex);
	return r;
}

/** @brief Internal: atomically set the global OpenAPI router reference.
 *
 * @param r Router to set (pass nullptr to disable the OpenAPI endpoint).
 * @note Thread-safe. The previous value is simply overwritten. */
static void
set_openapi_router(csilk_router_t* r)
{
	uv_mutex_lock(&s_app_mutex);
	s_openapi_router = r;
	uv_mutex_unlock(&s_app_mutex);
}

/** @brief Built-in handler for the /openapi.json endpoint.
 *
 * Retrieves the current OpenAPI router and serves the generated OpenAPI 3.0
 * specification as a JSON response. If the router is nullptr (endpoint disabled),
 * returns 404.
 *
 * @param c The request context. */
static void
openapi_handler(csilk_ctx_t* c)
{
	csilk_router_t* router = get_openapi_router();

	if (router) {
		csilk_serve_openapi(c,
				    router,
				    "csilk API",
				    CSILK_VERSION,
				    "Auto-generated OpenAPI 3.0 specification");
	} else {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
	}
}

/** @brief Built-in handler for the /docs endpoint — serves the Swagger UI HTML
 * page.
 *
 * Checks that the OpenAPI router is active, then serves the embedded Swagger
 * UI HTML page which loads /openapi.json at runtime.
 *
 * @param c The request context. */
static void
docs_handler(csilk_ctx_t* c)
{
	csilk_router_t* router = get_openapi_router();

	if (!router) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	csilk_serve_swagger_ui(c);
}

/** @brief Main application structure containing config, router, server, and
 * groups.
 *
 * Lifecycle: created in csilk_app_new(), destroyed in csilk_app_free().
 * Ownership: owns everything except the OpenAPI router pointer (global). */
struct csilk_app_s {
	csilk_config_t config;			 /**< Application config (port, logger settings,
                              *   server timeouts, CORS, etc.). Populated from
                              *   YAML or defaults in csilk_app_new(). */
	csilk_router_t* router;			 /**< Central router — all registered routes
                              *   (including static-file routes) converge here.
                              *   Created in csilk_app_new(), freed in
                              *   csilk_app_free(). */
	csilk_server_t* server;			 /**< libuv-based HTTP server. Wired to the router
                              *   at creation. Built-in middlewares (recovery,
                              *   logging) are injected via csilk_server_use(). */
	csilk_group_t* root_group;		 /**< Root route group with prefix "". All routes
                              *   added via csilk_app_add_route*() go through
                              *   this group. Freed in csilk_app_free(). */
	cached_group_t groups[CSILK_MAX_GROUPS]; /**< Prefix-to-group cache.
                                            *   Linear-scanned array; avoids
                                            *   duplicating groups for the same
                                            *   prefix string. Indexed by
                                            *   group_count. */
	int group_count; /**< Number of valid entries in groups[] (0..32). */
};

/* ---- global static-route table ---- */
static static_route_t g_static[CSILK_MAX_STATIC];
static int g_static_n = 0;

/* ===================================================================
 * internal helpers
 * =================================================================== */

/** @brief Find an existing group by prefix, or create a new one.
 *
 * ## Lookup strategy
 * 1. Root prefix ("", "/") — return root_group (lazy-created on first call).
 * 2. Linear scan groups[] cache — O(n) with n capped at CSILK_MAX_GROUPS (32).
 * 3. Cache miss — create a child group under root_group via
 *    csilk_group_group(), store in cache, return.
 *
 * Nesting under root_group means any middleware registered on root_group
 * automatically applies to all subgroup routes (see group.c: gather_handlers).
 *
 * @param app Application handle.
 * @param prefix URL path prefix.
 * @return Route group instance, or nullptr on failure. */
static csilk_group_t*
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
		CSILK_LOG_E("Failed to create route group: max group limit (%d) reached",
			    CSILK_MAX_GROUPS);
		return nullptr;
	}

	if (!app->root_group) {
		app->root_group = csilk_group_new(app->router, "");
		CSILK_LOG_D("Created root route group");
	}

	csilk_group_t* g = app->root_group ? csilk_group_group(app->root_group, prefix)
					   : csilk_group_new(app->router, prefix);
	if (!g) {
		CSILK_LOG_E("Failed to create subgroup for prefix: %s", prefix);
		return nullptr;
	}

	int n = app->group_count++;
	snprintf(app->groups[n].prefix, sizeof(app->groups[n].prefix), "%s", prefix);
	app->groups[n].group = g;
	CSILK_LOG_I("Created route group prefix: %s", prefix);
	return g;
}

/** @brief Internal static file serving handler.
 *
 * ## Dispatch algorithm
 * 1. Scan the global g_static[] table under the app mutex.
 * 2. Compare the request path against each url_prefix.
 * 3. On match: release the mutex, store the matched prefix in the context
 *    (for csilk_static to use as a path-stripping hint), and call
 *    csilk_static() with the mapped root_dir.
 * 4. If no prefix matches, return 404.
 *
 * The mutex is released before csilk_static() to avoid holding it during
 * disk I/O. The prefix match is a simple strncmp — the longest prefix
 * configured first wins.
 *
 * @param c The request context. */
static void
static_serve(csilk_ctx_t* c)
{
	const char* path = csilk_get_path(c);

	uv_mutex_lock(&s_app_mutex);
	int n = g_static_n;
	for (int i = 0; i < n; i++) {
		size_t plen = strlen(g_static[i].url_prefix);
		if (!strncmp(path, g_static[i].url_prefix, plen)) {
			const char* prefix = g_static[i].url_prefix;
			const char* root = g_static[i].root_dir;
			uv_mutex_unlock(&s_app_mutex);
			csilk_set(c, "static_prefix", (void*)prefix);
			csilk_static(c, root);
			return;
		}
	}
	uv_mutex_unlock(&s_app_mutex);
	csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
}

/* ===================================================================
 * public API
 * =================================================================== */

/** @brief Create a new application instance with optional YAML configuration
 * file.
 *
 * ## Bootstrap sequence (step-by-step)
 *
 *   Phase 1 — Config & Logging
 *   1. Allocate app struct (calloc). Set up the process-level app mutex
 *      (uv_once, thread-safe for concurrent csilk_app_new calls).
 *   2. Load YAML config from config_path, or apply hard-coded defaults
 *      (port 8080, info-level logging, 5 s idle timeout, 30 s I/O timeouts,
 *       1 MB max body, 64 KB max header, 8 KB max URL, 100 headers, 128
 *       backlog, TCP_NODELAY on).
 *   3. Initialize the logger from config.
 *
 *   Phase 2 — Core objects
 *   4. Create router + server, wire them together.
 *   5. Push built-in middleware onto the server's global middleware chain:
 *      recovery handler first, then request logger. Order matters — recovery
 *      must wrap everything.
 *   6. Create the root route group (prefix "").
 *
 *   Phase 3 — Built-in endpoints
 *   7. Register the global OpenAPI router reference so /openapi.json works.
 *   8. Register /openapi.json (GET) — reads the router's OpenAPI spec.
 *   9. Register /docs (GET) — serves embedded Swagger UI HTML.
 *  10. Register /csdk-docs/ as a static file route pointing to the bundled
 *      Swagger UI assets.
 *
 * @param config_path Path to a YAML configuration file, or nullptr to use
 *                    defaults (port 8080, info-level logging to stdout).
 * @return A new csilk_app_t instance, or nullptr on initialization failure.
 * @note The returned app must be freed with csilk_app_free(). On failure,
 *       any partially allocated resources are cleaned up internally. */
csilk_app_t*
csilk_app_new(const char* config_path)
{
	csilk_app_t* app = calloc(1, sizeof(csilk_app_t));
	if (!app) {
		return nullptr;
	}

	uv_once(&s_app_mutex_once, init_app_mutex);
	memset(&app->config, 0, sizeof(app->config));

	if (config_path && csilk_load_config(config_path, &app->config) == 0) {
		CSILK_LOG_I("Loaded config from %s", config_path);
	} else {
		app->config.port = CSILK_DFL_PORT;
		app->config.logger.level = CSILK_LOG_INFO;
		app->config.logger.use_colors = -1;
		app->config.server.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
		app->config.server.read_timeout_ms = 30000;
		app->config.server.write_timeout_ms = 30000;
		app->config.server.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
		app->config.server.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
		app->config.server.max_url_size = CSILK_DEFAULT_MAX_URL_SIZE;
		app->config.server.max_headers_count = 100;
		app->config.server.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;
		app->config.server.tcp_nodelay = 1;
	}

	if (csilk_log_init(app->config.logger) != 0) {
		goto fail;
	}

	app->router = csilk_router_new();
	app->server = csilk_server_new(app->router);
	if (!app->router || !app->server) {
		goto fail;
	}

	csilk_server_set_config(app->server, &app->config.server);
	csilk_server_use(app->server, csilk_recovery_handler);
	csilk_server_use(app->server, csilk_logger_handler);

	app->root_group = csilk_group_new(app->router, "");
	if (!app->root_group) {
		goto fail;
	}

	/* Register built-in /openapi.json and /docs endpoints */
	set_openapi_router(app->router);
	{
		csilk_handler_t openapi_h[] = {openapi_handler};
		csilk_router_add_extended(
		    app->router,
		    "GET",
		    "/openapi.json",
		    openapi_h,
		    1,
		    "/openapi.json",
		    nullptr,
		    nullptr,
		    "OpenAPI Specification",
		    "Returns the OpenAPI 3.0 JSON specification for this API");
	}
	{
		csilk_handler_t docs_h[] = {docs_handler};
		csilk_router_add(app->router, "GET", "/docs", docs_h, 1);
	}
	/* Register static /csilk-docs/ serving the bundled Swagger UI files */
	csilk_app_static(app, "/csilk-docs", CSILK_SWAGGER_UI_DIR);

	CSILK_LOG_I("csilk app initialized");
	return app;

fail:
	if (app->router) {
		csilk_router_free(app->router);
	}
	csilk_server_free(app->server);
	csilk_config_free(&app->config);
	free(app);
	return nullptr;
}

/** @brief Free all application resources: server, router, groups, config, and
 * logger.
 *
 * ## Teardown order (reverse of init)
 * 1. Close logger — stops accepting log entries.
 * 2. Free server — joins worker threads, closes connections, stops libuv.
 * 3. Free cached child groups (groups[] array).
 * 4. Free root group.
 * 5. Free router — releases all registered routes and OpenAPI metadata.
 * 6. Free config — releases dynamically allocated strings (log path, etc.).
 * 7. Free the app struct itself.
 *
 * Server must be freed before the router because the server holds a
 * reference to the router internally.
 *
 * @param app The application instance to free (may be nullptr).
 * @note Safe to call with nullptr. After this call the app pointer is invalid. */
void
csilk_app_free(csilk_app_t* app)
{
	if (!app) {
		return;
	}
	csilk_log_close();
	csilk_server_free(app->server);
	for (int i = 0; i < app->group_count; i++) {
		csilk_group_free(app->groups[i].group);
	}
	if (app->root_group) {
		csilk_group_free(app->root_group);
	}
	csilk_router_free(app->router);
	csilk_config_free(&app->config);
	free(app);
}

/* ---- logger ---- */

/** @brief Set the minimum log level for the global logger.
 *
 * Reinitializes the logger with the updated level. Messages below this
 * level are filtered out.
 *
 * @param app Application instance.
 * @param level  New minimum log level (e.g., CSILK_LOG_DEBUG). */
void
csilk_app_log_level(csilk_app_t* app, csilk_log_level_t level)
{
	if (!app) {
		return;
	}
	app->config.logger.level = level;
	(void)csilk_log_init(app->config.logger);
}

/** @brief Enable logging to a file with an optional rotation threshold.
 *
 * Sets the log output to the specified file path. If max_sz > 0, the log
 * file is rotated (renamed to .1) when it exceeds this size.
 *
 * @param app   Application instance.
 * @param path  File path for log output. Pass nullptr to disable file logging
 *              and revert to stdout.
 * @param max_sz Maximum file size in bytes before rotation (0 = no limit). */
void
csilk_app_log_file(csilk_app_t* app, const char* path, size_t max_sz)
{
	if (!app) {
		return;
	}
	if (app->config.logger.file_path) {
		free((void*)app->config.logger.file_path);
	}
	app->config.logger.file_path = path ? strdup(path) : nullptr;
	app->config.logger.max_file_size = max_sz;
	(void)csilk_log_init(app->config.logger);
}

/** @brief Enable or disable structured JSON log output format.
 *
 * When enabled, log entries are emitted as JSON objects with structured
 * fields (timestamp, level, file, line, message, request_id). When disabled,
 * plain text format is used.
 *
 * @param app    Application instance.
 * @param enable 1 for JSON format, 0 for plain text. */
void
csilk_app_log_json(csilk_app_t* app, int enable)
{
	if (!app) {
		return;
	}
	app->config.logger.json_format = enable;
	(void)csilk_log_init(app->config.logger);
}

/* ---- middleware ---- */

/** @brief Register a middleware handler that applies to all routes globally.
 *
 * Global middleware runs before route-specific middleware and handlers for
 * every request. Built-in recovery and logger middleware are registered
 * automatically by csilk_app_new().
 *
 * @param app Application instance.
 * @param h   Middleware handler function.
 * @note There is a hard limit of 32 global middlewares. */
void
csilk_app_use(csilk_app_t* app, csilk_handler_t h)
{
	if (!app || !app->server) {
		return;
	}
	csilk_server_use(app->server, h);
	CSILK_LOG_I("Registered global middleware: %p", (void*)h);
}

/** @brief Register a middleware handler scoped to a specific URL prefix group.
 *
 * Creates (or finds) a route group for the given prefix and adds the
 * middleware to it. The middleware runs for any route whose path starts
 * with the given prefix.
 *
 * @param app    Application instance.
 * @param prefix URL prefix (e.g., "/api/admin").
 * @param h      Middleware handler function. */
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
 * @param app Application instance. */
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

/* ---- OpenAPI / Swagger ---- */

/** @brief Enable or disable the built-in /openapi.json endpoint.
 *
 * When enabled, the router's routes are exposed as an OpenAPI 3.0
 * specification at /openapi.json. When disabled, the endpoint returns 404.
 *
 * @param app    Application instance.
 * @param enable 1 to enable, 0 to disable. */
void
csilk_app_enable_openapi(csilk_app_t* app, int enable)
{
	(void)app;
	set_openapi_router(enable ? app->router : nullptr);
	CSILK_LOG_I("OpenAPI endpoint %s", enable ? "enabled" : "disabled");
}

static csilk_group_t*
find_matching_group_for_path(csilk_app_t* app, const char* path, const char** out_relative_path)
{
	csilk_group_t* best_group = app->root_group;
	size_t best_len = 0;
	*out_relative_path = path;

	CSILK_LOG_T("Matching path '%s' against %d registered groups", path, app->group_count);

	for (int i = 0; i < app->group_count; i++) {
		const char* prefix = app->groups[i].prefix;
		size_t prefix_len = strlen(prefix);
		if (prefix_len > best_len && strncmp(path, prefix, prefix_len) == 0) {
			/* Ensure it's a clean boundary, e.g. /api matches /api/test but not /apipending */
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

/* ---- routes ---- */

/** @brief Register a route on the root group with a single handler.
 *
 * @param app    Application instance.
 * @param method HTTP method (e.g., "GET", "POST").
 * @param path   URL path (e.g., "/users").
 * @param handler Handler function. */
void
csilk_app_add_route(csilk_app_t* app, const char* method, const char* path, csilk_handler_t handler)
{
	if (!app || !method || !path || !handler) {
		return;
	}
	const char* relative_path = nullptr;
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
 * @param description Detailed description for the OpenAPI operation. */
void
csilk_app_add_route_extended(csilk_app_t* app,
			     const char* method,
			     const char* path,
			     csilk_handler_t handler,
			     const char* input_type,
			     const char* output_type,
			     const char* summary,
			     const char* description)
{
	if (!app || !method || !path || !handler) {
		return;
	}
	const char* relative_path = nullptr;
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
 *  @param perm_required  Permission required for this route, or nullptr.
 *  @param perm_resource  Resource pattern for permission check, or nullptr. */
void
csilk_app_add_route_extended_perm(csilk_app_t* app,
				  const char* method,
				  const char* path,
				  csilk_handler_t handler,
				  const char* input_type,
				  const char* output_type,
				  const char* summary,
				  const char* description,
				  const char* perm_required,
				  const char* perm_resource)
{
	if (!app || !method || !path || !handler) {
		return;
	}
	const char* relative_path = nullptr;
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
 *  @param perm_required  Permission identifier (e.g., "read"), or nullptr.
 *  @param perm_resource  Resource pattern (e.g., "users:*"), or nullptr. */
void
csilk_app_add_route_perm(csilk_app_t* app,
			 const char* method,
			 const char* path,
			 csilk_handler_t handler,
			 const char* perm_required,
			 const char* perm_resource)
{
	csilk_app_add_route_extended_perm(app,
					  method,
					  path,
					  handler,
					  nullptr,
					  nullptr,
					  nullptr,
					  nullptr,
					  perm_required,
					  perm_resource);
}

/** @brief Register a route with a custom handler chain on the root group.
 *
 * @param app      Application instance.
 * @param method   HTTP method.
 * @param path     URL path.
 * @param handlers Array of handler functions.
 * @param n        Number of handlers in the array. */
void
csilk_app_add_handlers(
    csilk_app_t* app, const char* method, const char* path, csilk_handler_t* handlers, size_t n)
{
	if (!app || !method || !path || !handlers || n == 0) {
		return;
	}
	const char* relative_path = nullptr;
	csilk_group_t* g = find_matching_group_for_path(app, path, &relative_path);
	if (!g) {
		CSILK_LOG_E("Failed to add handler chain %s %s: group match failed", method, path);
		return;
	}
	csilk_group_add_handlers(g, method, relative_path, handlers, n);
	CSILK_LOG_I("Route chain registered: %s %s (handlers count: %zu)", method, path, n);
}

/* ---- static files ---- */

/** @brief Configure static file serving: map a URL prefix to a local filesystem
 * directory.
 *
 * ## What it does
 * 1. Acquires the app mutex, checks the g_static[] table capacity (max 32).
 * 2. Writes url_prefix + root_dir into the next free global slot.
 * 3. Releases mutex, then registers two GET routes on the group matching
 *    `prefix`:
 *      - `WILDCARD path` — wildcard route that captures everything after the prefix.
 *      - `/`      — the prefix root (redirects to the index file).
 *    Both routes use the same internal static_serve handler, which scans
 *    g_static[] at request time to find the correct root_dir.
 *
 * The dual-route pattern means both `/static/` and `/static/foo/bar.jpg`
 * work. The `static_prefix` context variable lets csilk_static() strip the
 * URL prefix from the filesystem path.
 *
 * @param app      Application instance.
 * @param prefix   URL path prefix for static files (e.g., "/static").
 * @param root_dir Local filesystem directory to serve files from.
 * @note Routes are added using the app's internal static_serve handler which
 *       dispatches to csilk_static() with the correct root directory. */
void
csilk_app_static(csilk_app_t* app, const char* prefix, const char* root_dir)
{
	if (!app || !prefix || !root_dir) {
		return;
	}

	uv_once(&s_app_mutex_once, init_app_mutex);
	uv_mutex_lock(&s_app_mutex);
	if (g_static_n >= CSILK_MAX_STATIC) {
		uv_mutex_unlock(&s_app_mutex);
		CSILK_LOG_E(
		    "Static route limit (%d) reached. Route dropped: %s", CSILK_MAX_STATIC, prefix);
		return;
	}

	int idx = g_static_n++;
	snprintf(g_static[idx].url_prefix, sizeof(g_static[idx].url_prefix), "%s", prefix);
	snprintf(g_static[idx].root_dir, sizeof(g_static[idx].root_dir), "%s", root_dir);
	uv_mutex_unlock(&s_app_mutex);

	char wild[] = "/*path";
	char idxrt[] = "/";

	csilk_group_t* g = find_or_create_group(app, prefix);
	if (!g) {
		return;
	}

	csilk_handler_t hs[] = {static_serve, nullptr};
	csilk_group_add_handlers(g, "GET", wild, hs, 1);
	csilk_group_add_handlers(g, "GET", idxrt, hs, 1);

	CSILK_LOG_I("static: %s -> %s", prefix, root_dir);
}

/* ---- config / run / accessors ---- */

/** @brief Apply server-level configuration to the running server.
 *
 * Updates both the app's stored config and applies it to the server
 * instance. This overrides any previous server config.
 *
 * @param app Application instance.
 * @param c   Server configuration struct. */
void
csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c)
{
	if (!app || !app->server) {
		return;
	}
	app->config.server = c;
	csilk_server_set_config(app->server, &app->config.server);
}

/** @brief Get a heap-allocated copy of the current application configuration.
 *
 * @param app Application instance.
 * @return A malloc'd copy of the configuration struct. The caller must free
 *         it with free(). Returns nullptr if app is nullptr or allocation fails.
 * @note The returned copy includes deep copies of any dynamically allocated
 *       string fields? No — it is a shallow memcpy. Use csilk_config_free()
 *       only if you modify strings separately. */
csilk_config_t*
csilk_app_config(csilk_app_t* app)
{
	if (!app) {
		return nullptr;
	}
	csilk_config_t* cp = malloc(sizeof(csilk_config_t));
	if (cp) {
		memcpy(cp, &app->config, sizeof(csilk_config_t));
	}
	return cp;
}

/** @brief Start the server and enter the libuv event loop (blocking).
 *
 * This is the main entry point into the event-driven I/O loop. It delegates
 * to csilk_server_run() which:
 *   1. Creates a TCP listener on the given port.
 *   2. Spawns worker threads (if thread pool is configured).
 *   3. Calls uv_run(UV_RUN_DEFAULT) — blocks until the loop stops.
 *
 * @param app  Application instance.
 * @param port TCP port to listen on. Pass 0 or negative to use the port
 *             from the application config (default 8080).
 * @return The uv_run() return value on exit, or -1 on error. */
int
csilk_app_run(csilk_app_t* app, int port)
{
	if (!app) {
		return -1;
	}
	int p = port > 0 ? port : app->config.port;
	CSILK_LOG_I("\n  csilk server listening on http://localhost:%d\n\n", p);
	return csilk_server_run(app->server, p);
}

/** @brief Get the underlying router handle from the application.
 *
 * @param app Application instance.
 * @return The router pointer, or nullptr if app is nullptr. */
csilk_router_t*
csilk_app_router(csilk_app_t* app)
{
	return app ? app->router : nullptr;
}

/** @brief Get the underlying server handle from the application.
 *
 * @param app Application instance.
 * @return The server pointer, or nullptr if app is nullptr. */
csilk_server_t*
csilk_app_server(csilk_app_t* app)
{
	return app ? app->server : nullptr;
}
