/**
 * @file app.c
 * @brief High-level convenience API — csilk_app_t implementation.
 *
 * Wraps router, server, config, logging and built-in middleware into
 * a single "app" handle with a clean, Express-like API.
 * @copyright MIT License
 * @version 0.2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk.h"
#include "csilk_app.h"

#define CSILK_MAX_GROUPS  32
#define CSILK_MAX_STATIC  8
#define CSILK_DFL_PORT    8080

/** @brief Cached route group lookup entry. */
typedef struct {
    char           prefix[128]; /**< URL path prefix. */
    csilk_group_t* group;       /**< Cached group handle. */
} cached_group_t;

/** @brief Descriptor for a static file serving route. */
typedef struct {
    char url_prefix[128]; /**< URL path prefix for static files. */
    char root_dir[256];   /**< Local filesystem directory path. */
} static_route_t;

/** @brief Main application structure containing config, router, server, and groups. */
struct csilk_app_s {
    csilk_config_t   config;                    /**< Application configuration. */
    csilk_router_t*  router;                    /**< Router instance. */
    csilk_server_t*  server;                    /**< Server instance. */
    csilk_group_t*   root_group;                /**< Root route group. */
    cached_group_t   groups[CSILK_MAX_GROUPS];  /**< Cached group table. */
    int              group_count;               /**< Number of cached groups. */
};

/* ---- global static-route table ---- */
static static_route_t g_static[CSILK_MAX_STATIC];
static int             g_static_n = 0;

/* ===================================================================
 * internal helpers
 * =================================================================== */

/** @brief Find an existing group by prefix, or create a new one.
 * @param app Application handle.
 * @param prefix URL path prefix.
 * @return Route group instance, or NULL on failure. */
static csilk_group_t* find_or_create_group(csilk_app_t* app,
                                            const char* prefix) {
    if (!prefix || !*prefix || !strcmp(prefix, "/")) {
        if (!app->root_group)
            app->root_group = csilk_group_new(app->router, "");
        return app->root_group;
    }
    for (int i = 0; i < app->group_count; i++)
        if (!strcmp(app->groups[i].prefix, prefix))
            return app->groups[i].group;

    if (app->group_count >= CSILK_MAX_GROUPS) return NULL;

    if (!app->root_group)
        app->root_group = csilk_group_new(app->router, "");

    csilk_group_t* g = app->root_group
        ? csilk_group_group(app->root_group, prefix)
        : csilk_group_new(app->router, prefix);
    if (!g) return NULL;

    int n = app->group_count++;
    snprintf(app->groups[n].prefix, sizeof(app->groups[n].prefix),
             "%s", prefix);
    app->groups[n].group = g;
    return g;
}

/** @brief Internal static file serving handler.
 * Dispatches to csilk_static based on URL prefix.
 * @param c The request context. */
static void static_serve(csilk_ctx_t* c) {
    const char* path = c->request.path;
    for (int i = 0; i < g_static_n; i++) {
        size_t plen = strlen(g_static[i].url_prefix);
        if (!strncmp(path, g_static[i].url_prefix, plen)) {
            csilk_static(c, g_static[i].root_dir);
            return;
        }
    }
    csilk_string(c, 404, "Not Found");
}

/* ===================================================================
 * public API
 * =================================================================== */

csilk_app_t* csilk_app_new(const char* config_path) {
    csilk_app_t* app = calloc(1, sizeof(csilk_app_t));
    if (!app) return NULL;

    memset(&app->config, 0, sizeof(app->config));

    if (config_path && csilk_load_config(config_path, &app->config) == 0) {
        CSILK_LOG_I("Loaded config from %s", config_path);
    } else {
        app->config.port                  = CSILK_DFL_PORT;
        app->config.logger.level          = CSILK_LOG_INFO;
        app->config.logger.use_colors     = -1;
        app->config.server.idle_timeout_ms = 5000;
        app->config.server.max_body_size   = 1024UL * 1024UL;
        app->config.server.max_header_size = 64UL * 1024UL;
        app->config.server.listen_backlog  = 128;
        app->config.server.tcp_nodelay     = 1;
    }

    csilk_log_init(app->config.logger);

    app->router     = csilk_router_new();
    app->server     = csilk_server_new(app->router);
    if (!app->router || !app->server) goto fail;

    csilk_server_set_config(app->server, app->config.server);
    csilk_server_use(app->server, csilk_recovery_handler);
    csilk_server_use(app->server, csilk_logger_handler);

    app->root_group = csilk_group_new(app->router, "");
    if (!app->root_group) goto fail;

    CSILK_LOG_I("csilk app initialized");
    return app;

fail:
    if (app->router) csilk_router_free(app->router);
    csilk_config_free(&app->config);
    free(app);
    return NULL;
}

void csilk_app_free(csilk_app_t* app) {
    if (!app) return;
    csilk_log_close();
    csilk_server_free(app->server);
    for (int i = 0; i < app->group_count; i++)
        csilk_group_free(app->groups[i].group);
    if (app->root_group) csilk_group_free(app->root_group);
    csilk_router_free(app->router);
    csilk_config_free(&app->config);
    free(app);
}

/* ---- logger ---- */

void csilk_app_log_level(csilk_app_t* app, csilk_log_level_t lv) {
    if (!app) return;
    app->config.logger.level = lv;
    csilk_log_init(app->config.logger);
}

void csilk_app_log_file(csilk_app_t* app, const char* path, size_t max_sz) {
    if (!app) return;
    if (app->config.logger.file_path)
        free((void*)app->config.logger.file_path);
    app->config.logger.file_path    = path ? strdup(path) : NULL;
    app->config.logger.max_file_size = max_sz;
    csilk_log_init(app->config.logger);
}

/* ---- middleware ---- */

void csilk_app_use(csilk_app_t* app, csilk_handler_t h) {
    if (!app || !app->server) return;
    csilk_server_use(app->server, h);
}

void csilk_app_use_group(csilk_app_t* app, const char* prefix,
                          csilk_handler_t h) {
    if (!app || !prefix) return;
    csilk_group_t* g = find_or_create_group(app, prefix);
    if (g) csilk_group_use(g, h);
}

void csilk_app_apply_config(csilk_app_t* app) {
    if (!app) return;
    if (app->config.static_files.enable && app->config.static_files.root_dir) {
        csilk_app_static(app,
            app->config.static_files.prefix ?
                app->config.static_files.prefix : "/static",
            app->config.static_files.root_dir);
    }
}

/* ---- routes ---- */

void csilk_app_add_route(csilk_app_t* app, const char* method,
                          const char* path, csilk_handler_t h) {
    if (!app || !method || !path || !h) return;
    csilk_group_t* g = app->root_group;
    if (!g) return;
    csilk_group_add_route(g, method, path, h);
}

void csilk_app_add_handlers(csilk_app_t* app, const char* method,
                             const char* path, csilk_handler_t* handlers,
                             size_t n) {
    if (!app || !method || !path || !handlers || n == 0) return;
    csilk_group_t* g = app->root_group;
    if (!g) return;
    csilk_group_add_handlers(g, method, path, handlers, n);
}

/* ---- static files ---- */

void csilk_app_static(csilk_app_t* app, const char* prefix,
                       const char* root_dir) {
    if (!app || !prefix || !root_dir) return;
    if (g_static_n >= CSILK_MAX_STATIC) return;

    int idx = g_static_n++;
    snprintf(g_static[idx].url_prefix,
             sizeof(g_static[idx].url_prefix), "%s", prefix);
    snprintf(g_static[idx].root_dir,
             sizeof(g_static[idx].root_dir), "%s", root_dir);

    char wild[256], idxrt[256];
    snprintf(wild, sizeof(wild), "%s/*path", prefix);
    snprintf(idxrt, sizeof(idxrt), "%s/", prefix);

    csilk_group_t* g = find_or_create_group(app, prefix);
    if (!g) return;

    csilk_handler_t hs[] = { static_serve, NULL };
    csilk_group_add_handlers(g, "GET", wild, hs, 1);
    csilk_group_add_handlers(g, "GET", idxrt, hs, 1);

    CSILK_LOG_I("static: %s -> %s", prefix, root_dir);
}

/* ---- config / run / accessors ---- */

void csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c) {
    if (!app || !app->server) return;
    app->config.server = c;
    csilk_server_set_config(app->server, c);
}

csilk_config_t* csilk_app_config(csilk_app_t* app) {
    if (!app) return NULL;
    csilk_config_t* cp = malloc(sizeof(csilk_config_t));
    if (cp) memcpy(cp, &app->config, sizeof(csilk_config_t));
    return cp;
}

int csilk_app_run(csilk_app_t* app, int port) {
    if (!app) return -1;
    int p = port > 0 ? port : app->config.port;
    printf("\n  csilk server listening on http://localhost:%d\n\n", p);
    fflush(stdout);
    return csilk_server_run(app->server, p);
}

csilk_router_t* csilk_app_router(csilk_app_t* app) {
    return app ? app->router : NULL;
}

csilk_server_t* csilk_app_server(csilk_app_t* app) {
    return app ? app->server : NULL;
}
