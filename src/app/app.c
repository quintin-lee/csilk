/**
 * @file app.c
 * @brief High-level convenience API — csilk_app_t lifecycle, config, logger,
 *        OpenAPI, and static files.
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
 * @copyright MIT License
 * @version 1.2.0
 */

#include "csilk/app/app.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define R_OK 4
#else
#include <unistd.h>
#endif

#include "csilk/core/plugin.h"
#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "app_internal.h"

/** @brief Default HTTP listen port. */
enum { CSILK_DFL_PORT = 8080 };

static csilk_app_t* ctx_app(csilk_ctx_t* c);

#define CSILK_MW_THUNK_DECL(N) static void _csilk_mw_thunk_shim_##N(csilk_ctx_t* c);
CSILK_MW_THUNK_DECL(0)
CSILK_MW_THUNK_DECL(1)
CSILK_MW_THUNK_DECL(2)
CSILK_MW_THUNK_DECL(3)
CSILK_MW_THUNK_DECL(4)
CSILK_MW_THUNK_DECL(5)
CSILK_MW_THUNK_DECL(6)
CSILK_MW_THUNK_DECL(7)
CSILK_MW_THUNK_DECL(8)
CSILK_MW_THUNK_DECL(9)
CSILK_MW_THUNK_DECL(10)
CSILK_MW_THUNK_DECL(11)
CSILK_MW_THUNK_DECL(12)
CSILK_MW_THUNK_DECL(13)
CSILK_MW_THUNK_DECL(14)
CSILK_MW_THUNK_DECL(15)
CSILK_MW_THUNK_DECL(16)
CSILK_MW_THUNK_DECL(17)
CSILK_MW_THUNK_DECL(18)
CSILK_MW_THUNK_DECL(19)
CSILK_MW_THUNK_DECL(20)
CSILK_MW_THUNK_DECL(21)
CSILK_MW_THUNK_DECL(22)
CSILK_MW_THUNK_DECL(23)
CSILK_MW_THUNK_DECL(24)
CSILK_MW_THUNK_DECL(25)
CSILK_MW_THUNK_DECL(26)
CSILK_MW_THUNK_DECL(27)
CSILK_MW_THUNK_DECL(28)
CSILK_MW_THUNK_DECL(29)
CSILK_MW_THUNK_DECL(30)
CSILK_MW_THUNK_DECL(31)
#undef CSILK_MW_THUNK_DECL

/** @brief Internal: table of the generated trampolines, indexed by thunk slot. */
static const csilk_handler_t s_mw_thunk_shims[CSILK_MAX_MW_THUNKS] = {
    _csilk_mw_thunk_shim_0,  _csilk_mw_thunk_shim_1,  _csilk_mw_thunk_shim_2,
    _csilk_mw_thunk_shim_3,  _csilk_mw_thunk_shim_4,  _csilk_mw_thunk_shim_5,
    _csilk_mw_thunk_shim_6,  _csilk_mw_thunk_shim_7,  _csilk_mw_thunk_shim_8,
    _csilk_mw_thunk_shim_9,  _csilk_mw_thunk_shim_10, _csilk_mw_thunk_shim_11,
    _csilk_mw_thunk_shim_12, _csilk_mw_thunk_shim_13, _csilk_mw_thunk_shim_14,
    _csilk_mw_thunk_shim_15, _csilk_mw_thunk_shim_16, _csilk_mw_thunk_shim_17,
    _csilk_mw_thunk_shim_18, _csilk_mw_thunk_shim_19, _csilk_mw_thunk_shim_20,
    _csilk_mw_thunk_shim_21, _csilk_mw_thunk_shim_22, _csilk_mw_thunk_shim_23,
    _csilk_mw_thunk_shim_24, _csilk_mw_thunk_shim_25, _csilk_mw_thunk_shim_26,
    _csilk_mw_thunk_shim_27, _csilk_mw_thunk_shim_28, _csilk_mw_thunk_shim_29,
    _csilk_mw_thunk_shim_30, _csilk_mw_thunk_shim_31,
};

static_assert(sizeof(s_mw_thunk_shims) / sizeof(s_mw_thunk_shims[0]) == CSILK_MAX_MW_THUNKS,
              "s_mw_thunk_shims must match CSILK_MAX_MW_THUNKS");

/** @brief Router reference for the built-in OpenAPI handler.
 * Now per-app: each app instance owns its own static-route table and
 * middleware thunk pool, so multiple apps in one process do not
 * interfere with each other. */

/** @brief Internal: recover the owning app instance from a request context.
 * Handlers registered via csilk_app_* are static functions that cannot
 * capture a csilk_app_t* directly, so they recover it through the
 * context's server handle. */
static csilk_app_t*
ctx_app(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    csilk_server_t* s = csilk_ctx_get_server(c);
    if (!s) {
        return NULL;
    }
    return (csilk_app_t*)s->app;
}

/** @brief Internal: safely retrieve the app's router if OpenAPI is enabled.
 * Returns the app's router if the OpenAPI endpoint is enabled, else NULL. */
static csilk_router_t*
app_openapi_router(csilk_app_t* app)
{
    if (!app || !atomic_load_explicit(&app->openapi_enabled, memory_order_acquire)) {
        return NULL;
    }
    return app->router;
}

/** @brief Built-in handler for the /openapi.json endpoint. */
static void
openapi_handler(csilk_ctx_t* c)
{
    csilk_app_t*    app = ctx_app(c);
    csilk_router_t* router = app_openapi_router(app);

    if (router) {
        csilk_serve_openapi(
            c, router, "csilk API", CSILK_VERSION, "Auto-generated OpenAPI 3.0 specification");
    } else {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    }
}

/** @brief Built-in handler for the /docs endpoint — serves the Swagger UI HTML
 * page. */
static void
docs_handler(csilk_ctx_t* c)
{
    csilk_app_t*    app = ctx_app(c);
    csilk_router_t* router = app_openapi_router(app);

    if (!router) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    csilk_serve_swagger_ui(c);
}

/** @brief Resolve the best directory path containing Swagger UI static distribution files. */
static const char*
resolve_swagger_ui_dir(void)
{
    const char* env_dir = getenv("CSILK_SWAGGER_UI_DIR");
    if (env_dir && access(env_dir, R_OK) == 0) {
        return env_dir;
    }
#ifdef CSILK_SWAGGER_UI_DIR
    if (access(CSILK_SWAGGER_UI_DIR, R_OK) == 0) {
        return CSILK_SWAGGER_UI_DIR;
    }
#endif
    static const char* const candidates[] = {"/usr/local/share/csilk/swagger-ui",
                                             "/usr/share/csilk/swagger-ui",
                                             "./share/swagger-ui",
                                             "../share/swagger-ui",
                                             "../../share/swagger-ui",
                                             NULL};
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
#ifdef CSILK_SWAGGER_UI_DIR
    return CSILK_SWAGGER_UI_DIR;
#else
    return NULL;
#endif
}

/** @brief Internal: check if a path contains directory traversal sequences. */
static int
contains_path_traversal(const char* path)
{
    if (!path) {
        return 1;
    }
    const char* p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            char prev = (p == path) ? '/' : *(p - 1);
            char next = *(p + 2);
            if ((prev == '/' || prev == '\0') && (next == '/' || next == '\0' || next == '\0')) {
                return 1;
            }
        }
        ++p;
    }
    return 0;
}

/** @brief Internal: serve a file from the matched static route prefix,
 * rejecting directory-traversal paths.  Now per-app: the static-route table
 * lives inside the owning csilk_app_t so multiple apps do not share state. */
static void
static_serve(csilk_ctx_t* c)
{
    const char* path = csilk_get_path(c);

    if (contains_path_traversal(path)) {
        CSILK_LOG_W("Static: blocked path traversal attempt: %s", path);
        csilk_string(c, CSILK_STATUS_FORBIDDEN, "Forbidden");
        return;
    }

    csilk_app_t* app = ctx_app(c);
    if (!app) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }

    csilk_mutex_lock(&app->app_mutex);
    int         n = app->static_n;
    const char* best_prefix = NULL;
    const char* best_root = NULL;
    size_t      best_len = 0;
    for (int i = 0; i < n; i++) {
        const char* pfx = app->static_routes[i].url_prefix;
        size_t      plen = strlen(pfx);
        if (strncmp(path, pfx, plen) == 0 &&
            (plen == 0 || pfx[plen - 1] == '/' || path[plen] == '\0' || path[plen] == '/')) {
            if (!best_prefix || plen > best_len) {
                best_len = plen;
                best_prefix = pfx;
                best_root = app->static_routes[i].root_dir;
            }
        }
    }
    if (best_prefix && best_root) {
        csilk_mutex_unlock(&app->app_mutex);
        csilk_set(c, "static_prefix", (void*)best_prefix);
        csilk_static(c, best_root);
        return;
    }
    csilk_mutex_unlock(&app->app_mutex);
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
}

/* ===================================================================
 * public API
 * =================================================================== */

/**
 * @brief Create a new application instance with optional YAML configuration
 *        file.
 * @param[in] config_path Path to a YAML config file, or NULL for defaults.
 * @return New csilk_app_t* on success, or NULL on allocation/config failure.
 */
csilk_app_t*
csilk_app_new(const char* config_path)
{
    csilk_app_t* app = calloc(1, sizeof(csilk_app_t));
    if (!app) {
        return NULL;
    }

    csilk_mutex_init(&app->app_mutex);
    atomic_init(&app->openapi_enabled, 1);
    atomic_init(&app->mw_thunk_used, 0);
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
    app->server->app = app; /* back-reference so handlers can recover the app */

    if (csilk_app_load_plugins_from_env(app) != 0) {
        CSILK_LOG_W("App: one or more plugins from CSILK_PLUGIN_PATHS failed to load");
    }

    csilk_server_set_config(app->server, &app->config.server);
    csilk_server_use(app->server, csilk_recovery_handler);
    csilk_server_use(app->server, csilk_logger_handler);

    app->root_group = csilk_group_new(app->router, "");
    if (!app->root_group) {
        goto fail;
    }

    /* Register built-in /openapi.json and /docs /swagger endpoints */
    {
        csilk_handler_t openapi_h[] = {openapi_handler};
        csilk_router_add_extended(app->router,
                                  "GET",
                                  "/openapi.json",
                                  openapi_h,
                                  1,
                                  "/openapi.json",
                                  NULL,
                                  NULL,
                                  "OpenAPI Specification",
                                  "Returns the OpenAPI 3.0 JSON specification for this API");
    }
    {
        csilk_handler_t docs_h[] = {docs_handler};
        csilk_router_add(app->router, "GET", "/docs", docs_h, 1);
        csilk_router_add(app->router, "GET", "/swagger", docs_h, 1);
        csilk_router_add(app->router, "GET", "/swagger-ui", docs_h, 1);
    }
    /* Register static /csilk-docs/ serving the bundled Swagger UI files if available */
    const char* swagger_dir = resolve_swagger_ui_dir();
    if (swagger_dir) {
        csilk_app_static(app, "/csilk-docs", swagger_dir);
    }

    CSILK_LOG_I("csilk app initialized");
    return app;

fail:
    csilk_server_free(app->server);
    if (app->root_group) {
        csilk_group_free(app->root_group);
    }
    if (app->router) {
        csilk_router_free(app->router);
    }
    csilk_config_free(&app->config);
    csilk_mutex_destroy(&app->app_mutex);
    free(app);
    return NULL;
}

/**
 * @brief Free all application resources: server, router, groups, config, and
 *        logger.
 * @param[in] app Application instance to free (may be NULL).
 */
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
    csilk_mutex_destroy(&app->app_mutex);
    extern void csilk_openapi_cache_invalidate(void);
    csilk_openapi_cache_invalidate();
    free(app);
}

/** @brief Load a plugin shared library into the app's server. */
int
csilk_app_load_plugin(csilk_app_t* app, const char* path)
{
    if (!app || !app->server || !path) {
        return -1;
    }
    return csilk_core_load_plugin(app->server, path);
}

/** @brief Load plugins from CSILK_PLUGIN_PATHS environment variable. */
int
csilk_app_load_plugins_from_env(csilk_app_t* app)
{
    if (!app || !app->server) {
        return -1;
    }

    const char* env = getenv("CSILK_PLUGIN_PATHS");
    if (!env || !*env) {
        return 0;
    }

    /* Copy the env string since strtok modifies it. */
    size_t len = strlen(env);
    char*  buf = malloc(len + 1);
    if (!buf) {
        return -1;
    }
    memcpy(buf, env, len + 1);

    int   rc = 0;
    char* saveptr = NULL;
    char* token = strtok_r(buf, ":", &saveptr);
    while (token != NULL) {
        if (csilk_core_load_plugin(app->server, token) != 0) {
            CSILK_LOG_W("Failed to load plugin: %s", token);
            rc = -1;
        }
        token = strtok_r(NULL, ":", &saveptr);
    }

    free(buf);
    return rc;
}

/* ---- logger ---- */

/**
 * @brief Set the application's log level and reinitialize the logger.
 * @param[in] app   Application instance.
 * @param[in] level New minimum log level to emit.
 */
void
csilk_app_log_level(csilk_app_t* app, csilk_log_level_t level)
{
    if (!app) {
        return;
    }
    app->config.logger.level = level;
    (void)csilk_log_init(app->config.logger);
}

/**
 * @brief Configure file-based logging for the application.
 * @param[in] app    Application instance.
 * @param[in] path   Log file path, or NULL to disable file logging.
 * @param[in] max_sz Maximum log file size in bytes before rotation.
 */
void
csilk_app_log_file(csilk_app_t* app, const char* path, size_t max_sz)
{
    if (!app) {
        return;
    }
    if (app->config.logger.file_path) {
        free((void*)app->config.logger.file_path);
    }
    app->config.logger.file_path = path ? strdup(path) : NULL;
    app->config.logger.max_file_size = max_sz;
    (void)csilk_log_init(app->config.logger);
}

/**
 * @brief Enable or disable JSON-structured log output.
 * @param[in] app    Application instance.
 * @param[in] enable Non-zero to enable JSON formatting, zero to disable.
 */
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

/** @brief Internal: thunk handler generated by csilk_app_use_mw() (csilk#9c).
 * Forwards to the captured dual-parameter middleware with the registered
 * user_data. */
/* ===================================================================
 * dual-parameter middleware thunk (csilk#9c)
 *
 * csilk_app_use() only accepts a csilk_handler_t (single ctx arg), so a
 * config-driven middleware such as csilk_cors_middleware() cannot be
 * registered without an out-of-band config pointer.  To bridge the gap
 * without global/static state, each csilk_app_use_mw() call captures its
 * (fn, user_data) pair in a process-wide static thunk slot and registers
 * a *distinct* generated trampoline that recovers its own slot and
 * dispatches.  A static trampoline table (CSILK_MAX_MW_THUNKS entries)
 * is emitted by a macro so each slot has its own function identity.
 * =================================================================== */

/** @brief Internal: per-app thunk pool — one slot per registered dual-parameter
 * middleware, bounded by CSILK_MAX_MW_THUNKS.
 * A trampoline table of CSILK_MAX_MW_THUNKS (32) generated shims is declared at
 * the top of this file; slots are allocated in order per app instance. */

/** @brief Internal: emit a distinct trampoline that dispatches to thunk slot
 * @p N.  The slot is resolved through the owning csilk_app_t (recovered from
 * the request context) so each app has its own bounded thunk pool. */
#define CSILK_MW_THUNK_SHIM(N)                                                                     \
    static void _csilk_mw_thunk_shim_##N(csilk_ctx_t* c)                                           \
    {                                                                                              \
        csilk_app_t* app = ctx_app(c);                                                             \
        if (app && atomic_load_explicit(&app->mw_thunk_used, memory_order_acquire) > (N)) {        \
            app->mw_thunk_pool[(N)].fn(c, app->mw_thunk_pool[(N)].user_data);                      \
        }                                                                                          \
    }

CSILK_MW_THUNK_SHIM(0)
CSILK_MW_THUNK_SHIM(1)
CSILK_MW_THUNK_SHIM(2)
CSILK_MW_THUNK_SHIM(3)
CSILK_MW_THUNK_SHIM(4)
CSILK_MW_THUNK_SHIM(5)
CSILK_MW_THUNK_SHIM(6)
CSILK_MW_THUNK_SHIM(7)
CSILK_MW_THUNK_SHIM(8)
CSILK_MW_THUNK_SHIM(9)
CSILK_MW_THUNK_SHIM(10)
CSILK_MW_THUNK_SHIM(11)
CSILK_MW_THUNK_SHIM(12)
CSILK_MW_THUNK_SHIM(13)
CSILK_MW_THUNK_SHIM(14)
CSILK_MW_THUNK_SHIM(15)
CSILK_MW_THUNK_SHIM(16)
CSILK_MW_THUNK_SHIM(17)
CSILK_MW_THUNK_SHIM(18)
CSILK_MW_THUNK_SHIM(19)
CSILK_MW_THUNK_SHIM(20)
CSILK_MW_THUNK_SHIM(21)
CSILK_MW_THUNK_SHIM(22)
CSILK_MW_THUNK_SHIM(23)
CSILK_MW_THUNK_SHIM(24)
CSILK_MW_THUNK_SHIM(25)
CSILK_MW_THUNK_SHIM(26)
CSILK_MW_THUNK_SHIM(27)
CSILK_MW_THUNK_SHIM(28)
CSILK_MW_THUNK_SHIM(29)
CSILK_MW_THUNK_SHIM(30)
CSILK_MW_THUNK_SHIM(31)
#undef CSILK_MW_THUNK_SHIM

/**
 * @brief Register a dual-parameter middleware with a config pointer (csilk#9c).
 * @param[in] app Application instance.
 * @param[in] mw  Dual-parameter middleware handler.
 * @param[in] user_data Config pointer forwarded to @p mw on every request.
 */
void
csilk_app_use_mw(csilk_app_t* app, csilk_middleware_t mw, void* user_data)
{
    if (!app || !app->server || !mw) {
        return;
    }
    csilk_mutex_lock(&app->app_mutex);
    int slot = atomic_load_explicit(&app->mw_thunk_used, memory_order_relaxed);
    if (slot >= CSILK_MAX_MW_THUNKS) {
        csilk_mutex_unlock(&app->app_mutex);
        CSILK_LOG_E("App: dual-parameter middleware shim limit (%d) reached. Dropped: %p",
                    CSILK_MAX_MW_THUNKS,
                    (void*)mw);
        return;
    }
    app->mw_thunk_pool[slot].fn = mw;
    app->mw_thunk_pool[slot].user_data = user_data;
    atomic_store_explicit(&app->mw_thunk_used, slot + 1, memory_order_release);

    csilk_handler_t shim = s_mw_thunk_shims[slot];
    if (csilk_server_use(app->server, shim) != 0) {
        atomic_store_explicit(&app->mw_thunk_used, slot, memory_order_release); /* roll back slot */
        csilk_mutex_unlock(&app->app_mutex);
        CSILK_LOG_E("App: failed to register dual-parameter middleware %p", (void*)mw);
        return;
    }
    csilk_mutex_unlock(&app->app_mutex);
    CSILK_LOG_I(
        "Registered dual-parameter middleware: %p data=%p slot=%d", (void*)mw, user_data, slot);
}

void
csilk_app_use(csilk_app_t* app, csilk_handler_t h)
{
    if (!app || !app->server) {
        return;
    }
    csilk_server_use(app->server, h);
    CSILK_LOG_I("Registered global middleware: %p", (void*)h);
}

int
csilk_app_use_wasm(csilk_app_t* app, csilk_wasm_plugin_t* plugin, const char* func_name)
{
    if (!app || !plugin || !func_name) {
        return -1;
    }
    csilk_handler_t mw = csilk_wasm_plugin_middleware(plugin, func_name);
    if (!mw) {
        return -1;
    }
    csilk_app_use(app, mw);
    return 0;
}

/* ---- OpenAPI / Swagger ---- */

/**
 * @brief Enable or disable the built-in OpenAPI/Swagger endpoints.
 * @param[in] app    Application instance.
 * @param[in] enable Non-zero to expose /openapi.json and /docs, zero to hide.
 */
void
csilk_app_enable_openapi(csilk_app_t* app, int enable)
{
    if (!app) {
        return;
    }
    atomic_store_explicit(&app->openapi_enabled, enable, memory_order_release);
    CSILK_LOG_I("OpenAPI endpoint %s", enable ? "enabled" : "disabled");
}

/* ---- static files ---- */

/**
 * @brief Configure static file serving: map a URL prefix to a local filesystem
 *        directory.
 * @param[in] app      Application instance.
 * @param[in] prefix   URL prefix to serve static files under (e.g., "/static").
 * @param[in] root_dir Filesystem directory to serve files from.
 */
void
csilk_app_static(csilk_app_t* app, const char* prefix, const char* root_dir)
{
    if (!app || !prefix || !root_dir) {
        return;
    }

    csilk_mutex_lock(&app->app_mutex);
    if (app->static_n >= CSILK_MAX_STATIC) {
        csilk_mutex_unlock(&app->app_mutex);
        CSILK_LOG_E("Static route limit (%d) reached. Route dropped: %s", CSILK_MAX_STATIC, prefix);
        return;
    }

    int idx = app->static_n++;
    snprintf(app->static_routes[idx].url_prefix,
             sizeof(app->static_routes[idx].url_prefix),
             "%s",
             prefix);
    snprintf(
        app->static_routes[idx].root_dir, sizeof(app->static_routes[idx].root_dir), "%s", root_dir);
    csilk_mutex_unlock(&app->app_mutex);

    char wild[] = "/*path";
    char idxrt[] = "/";

    csilk_group_t* g = find_or_create_group(app, prefix);
    if (!g) {
        return;
    }

    csilk_handler_t hs[] = {static_serve, NULL};
    csilk_group_add_handlers(g, "GET", wild, hs, 1);
    csilk_group_add_handlers(g, "GET", idxrt, hs, 1);

    CSILK_LOG_I("static: %s -> %s", prefix, root_dir);
}

/* ---- config / run / accessors ---- */

/**
 * @brief Replace the application's server configuration and apply it.
 * @param[in] app Application instance.
 * @param[in] c   New server configuration (copied by value).
 */
void
csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c)
{
    if (!app || !app->server) {
        return;
    }
    app->config.server = c;
    csilk_server_set_config(app->server, &app->config.server);
}

/**
 * @brief Return a snapshot copy of the application's configuration.
 * @param[in] app Application instance.
 * @return Heap-allocated csilk_config_t* copy, or NULL on error. Caller frees.
 */
csilk_config_t*
csilk_app_config(csilk_app_t* app)
{
    if (!app) {
        return NULL;
    }
    csilk_config_t* cp = malloc(sizeof(csilk_config_t));
    if (cp) {
        memcpy(cp, &app->config, sizeof(csilk_config_t));
    }
    return cp;
}

/**
 * @brief Start the HTTP server and block while serving requests.
 * @param[in] app  Application instance.
 * @param[in] port Listen port; if <= 0, the configured port is used.
 * @return 0 on normal shutdown, or a negative error code on failure.
 */
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

/**
 * @brief Get the router owned by the application.
 * @param[in] app Application instance.
 * @return The app's csilk_router_t*, or NULL if app is NULL.
 */
csilk_router_t*
csilk_app_router(csilk_app_t* app)
{
    return app ? app->router : NULL;
}

/**
 * @brief Get the server owned by the application.
 * @param[in] app Application instance.
 * @return The app's csilk_server_t*, or NULL if app is NULL.
 */
csilk_server_t*
csilk_app_server(csilk_app_t* app)
{
    return app ? app->server : NULL;
}
