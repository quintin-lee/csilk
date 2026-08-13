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
 * @version 0.4.0
 */

#include "csilk/app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "app_internal.h"

/** @brief Maximum number of static file routes per app. */
enum { CSILK_MAX_STATIC = 32 };
/** @brief Default HTTP listen port. */
enum { CSILK_DFL_PORT = 8080 };

/** @brief Internal: descriptor for a static file serving route mapping URL
 * prefix to filesystem directory. */
typedef struct {
    char url_prefix[128];
    char root_dir[256];
} static_route_t;

/** @brief Router reference for the built-in OpenAPI handler. */
static csilk_router_t* s_openapi_router = nullptr;
static csilk_mutex_t   s_app_mutex;
static csilk_once_t    s_app_mutex_once = CSILK_ONCE_INIT;

/** @brief Internal: initialize the application-level mutex (called once via
 * csilk_once). */
static void
init_app_mutex(void)
{
    csilk_mutex_init(&s_app_mutex);
}

/** @brief Internal: safely retrieve the current OpenAPI router under the app
 * mutex. */
static csilk_router_t*
get_openapi_router(void)
{
    csilk_mutex_lock(&s_app_mutex);
    csilk_router_t* r = s_openapi_router;
    csilk_mutex_unlock(&s_app_mutex);
    return r;
}

/** @brief Internal: atomically set the global OpenAPI router reference. */
static void
set_openapi_router(csilk_router_t* r)
{
    csilk_mutex_lock(&s_app_mutex);
    s_openapi_router = r;
    csilk_mutex_unlock(&s_app_mutex);
}

/** @brief Built-in handler for the /openapi.json endpoint. */
static void
openapi_handler(csilk_ctx_t* c)
{
    csilk_router_t* router = get_openapi_router();

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
    csilk_router_t* router = get_openapi_router();

    if (!router) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    csilk_serve_swagger_ui(c);
}

/* ---- global static-route table ---- */
static static_route_t g_static[CSILK_MAX_STATIC];
static int            g_static_n = 0;

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

static void
static_serve(csilk_ctx_t* c)
{
    const char* path = csilk_get_path(c);

    if (contains_path_traversal(path)) {
        CSILK_LOG_W("Static: blocked path traversal attempt: %s", path);
        csilk_string(c, CSILK_STATUS_FORBIDDEN, "Forbidden");
        return;
    }

    csilk_mutex_lock(&s_app_mutex);
    int n = g_static_n;
    for (int i = 0; i < n; i++) {
        size_t plen = strlen(g_static[i].url_prefix);
        if (!strncmp(path, g_static[i].url_prefix, plen)) {
            const char* prefix = g_static[i].url_prefix;
            const char* root = g_static[i].root_dir;
            csilk_mutex_unlock(&s_app_mutex);
            csilk_set(c, "static_prefix", (void*)prefix);
            csilk_static(c, root);
            return;
        }
    }
    csilk_mutex_unlock(&s_app_mutex);
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
}

/* ===================================================================
 * public API
 * =================================================================== */

/** @brief Create a new application instance with optional YAML configuration
 * file. */
csilk_app_t*
csilk_app_new(const char* config_path)
{
    csilk_app_t* app = calloc(1, sizeof(csilk_app_t));
    if (!app) {
        return nullptr;
    }

    csilk_once(&s_app_mutex_once, init_app_mutex);
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
        csilk_router_add_extended(app->router,
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
 * logger. */
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

void
csilk_app_log_level(csilk_app_t* app, csilk_log_level_t level)
{
    if (!app) {
        return;
    }
    app->config.logger.level = level;
    (void)csilk_log_init(app->config.logger);
}

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

/** @brief Register a middleware handler that applies to all routes globally. */
void
csilk_app_use(csilk_app_t* app, csilk_handler_t h)
{
    if (!app || !app->server) {
        return;
    }
    csilk_server_use(app->server, h);
    CSILK_LOG_I("Registered global middleware: %p", (void*)h);
}

/* ---- OpenAPI / Swagger ---- */

void
csilk_app_enable_openapi(csilk_app_t* app, int enable)
{
    (void)app;
    set_openapi_router(enable ? app->router : nullptr);
    CSILK_LOG_I("OpenAPI endpoint %s", enable ? "enabled" : "disabled");
}

/* ---- static files ---- */

/** @brief Configure static file serving: map a URL prefix to a local filesystem
 * directory. */
void
csilk_app_static(csilk_app_t* app, const char* prefix, const char* root_dir)
{
    if (!app || !prefix || !root_dir) {
        return;
    }

    csilk_once(&s_app_mutex_once, init_app_mutex);
    csilk_mutex_lock(&s_app_mutex);
    if (g_static_n >= CSILK_MAX_STATIC) {
        csilk_mutex_unlock(&s_app_mutex);
        CSILK_LOG_E("Static route limit (%d) reached. Route dropped: %s", CSILK_MAX_STATIC, prefix);
        return;
    }

    int idx = g_static_n++;
    snprintf(g_static[idx].url_prefix, sizeof(g_static[idx].url_prefix), "%s", prefix);
    snprintf(g_static[idx].root_dir, sizeof(g_static[idx].root_dir), "%s", root_dir);
    csilk_mutex_unlock(&s_app_mutex);

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

void
csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c)
{
    if (!app || !app->server) {
        return;
    }
    app->config.server = c;
    csilk_server_set_config(app->server, &app->config.server);
}

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

csilk_router_t*
csilk_app_router(csilk_app_t* app)
{
    return app ? app->router : nullptr;
}

csilk_server_t*
csilk_app_server(csilk_app_t* app)
{
    return app ? app->server : nullptr;
}
