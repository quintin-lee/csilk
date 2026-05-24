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

#define CSILK_MAX_GROUPS 32
#define CSILK_MAX_STATIC 32
#define CSILK_DFL_PORT 8080

/** @brief Cached route group lookup entry. */
typedef struct {
  char prefix[128];     /**< URL path prefix. */
  csilk_group_t* group; /**< Cached group handle. */
} cached_group_t;

/** @brief Descriptor for a static file serving route. */
typedef struct {
  char url_prefix[128]; /**< URL path prefix for static files. */
  char root_dir[256];   /**< Local filesystem directory path. */
} static_route_t;

/** @brief Router reference for the built-in OpenAPI handler. */
static csilk_router_t* s_openapi_router = NULL;
static uv_mutex_t s_app_mutex;
static uv_once_t s_app_mutex_once = UV_ONCE_INIT;

/** @brief Initialize the app-level mutex once. */
static void init_app_mutex(void) { uv_mutex_init(&s_app_mutex); }

/** @brief Lock the app mutex for reading s_openapi_router. */
static csilk_router_t* get_openapi_router(void) {
  uv_mutex_lock(&s_app_mutex);
  csilk_router_t* r = s_openapi_router;
  uv_mutex_unlock(&s_app_mutex);
  return r;
}

/** @brief Set the OpenAPI router under lock. */
static void set_openapi_router(csilk_router_t* r) {
  uv_mutex_lock(&s_app_mutex);
  s_openapi_router = r;
  uv_mutex_unlock(&s_app_mutex);
}

/** @brief Built-in handler for /openapi.json endpoint. */
static void openapi_handler(csilk_ctx_t* c) {
  csilk_router_t* router = get_openapi_router();

  if (router) {
    csilk_serve_openapi(c, router, "csilk API", CSILK_VERSION,
                        "Auto-generated OpenAPI 3.0 specification");
  } else {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
  }
}

/** @brief Built-in handler for /docs endpoint - serves the Swagger UI page. */
static void docs_handler(csilk_ctx_t* c) {
  csilk_router_t* router = get_openapi_router();

  if (!router) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }
  csilk_serve_swagger_ui(c);
}

/** @brief Main application structure containing config, router, server, and
 * groups. */
struct csilk_app_s {
  csilk_config_t config;                   /**< Application configuration. */
  csilk_router_t* router;                  /**< Router instance. */
  csilk_server_t* server;                  /**< Server instance. */
  csilk_group_t* root_group;               /**< Root route group. */
  cached_group_t groups[CSILK_MAX_GROUPS]; /**< Cached group table. */
  int group_count;                         /**< Number of cached groups. */
};

/* ---- global static-route table ---- */
static static_route_t g_static[CSILK_MAX_STATIC];
static int g_static_n = 0;

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
    if (!app->root_group) app->root_group = csilk_group_new(app->router, "");
    return app->root_group;
  }
  for (int i = 0; i < app->group_count; i++)
    if (!strcmp(app->groups[i].prefix, prefix)) return app->groups[i].group;

  if (app->group_count >= CSILK_MAX_GROUPS) return NULL;

  if (!app->root_group) app->root_group = csilk_group_new(app->router, "");

  csilk_group_t* g = app->root_group
                         ? csilk_group_group(app->root_group, prefix)
                         : csilk_group_new(app->router, prefix);
  if (!g) return NULL;

  int n = app->group_count++;
  snprintf(app->groups[n].prefix, sizeof(app->groups[n].prefix), "%s", prefix);
  app->groups[n].group = g;
  return g;
}

/** @brief Internal static file serving handler.
 * Dispatches to csilk_static based on URL prefix.
 * @param c The request context. */
static void static_serve(csilk_ctx_t* c) {
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

/** @brief Create a new application with optional YAML configuration. */
csilk_app_t* csilk_app_new(const char* config_path) {
  csilk_app_t* app = calloc(1, sizeof(csilk_app_t));
  if (!app) return NULL;

  uv_once(&s_app_mutex_once, init_app_mutex);
  memset(&app->config, 0, sizeof(app->config));

  if (config_path && csilk_load_config(config_path, &app->config) == 0) {
    CSILK_LOG_I("Loaded config from %s", config_path);
  } else {
    app->config.port = CSILK_DFL_PORT;
    app->config.logger.level = CSILK_LOG_INFO;
    app->config.logger.use_colors = -1;
    app->config.server.idle_timeout_ms = 5000;
    app->config.server.read_timeout_ms = 30000;
    app->config.server.write_timeout_ms = 30000;
    app->config.server.max_body_size = 1024UL * 1024UL;
    app->config.server.max_header_size = 64UL * 1024UL;
    app->config.server.max_url_size = 8192;
    app->config.server.max_headers_count = 100;
    app->config.server.listen_backlog = 128;
    app->config.server.tcp_nodelay = 1;
  }

  if (csilk_log_init(app->config.logger) != 0) {
    goto fail;
  }

  app->router = csilk_router_new();
  app->server = csilk_server_new(app->router);
  if (!app->router || !app->server) goto fail;

  csilk_server_set_config(app->server, &app->config.server);
  csilk_server_use(app->server, csilk_recovery_handler);
  csilk_server_use(app->server, csilk_logger_handler);

  app->root_group = csilk_group_new(app->router, "");
  if (!app->root_group) goto fail;

  /* Register built-in /openapi.json and /docs endpoints */
  set_openapi_router(app->router);
  {
    csilk_handler_t openapi_h[] = {openapi_handler};
    csilk_router_add_extended(
        app->router, "GET", "/openapi.json", openapi_h, 1, "/openapi.json",
        NULL, NULL, "OpenAPI Specification",
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
  if (app->router) csilk_router_free(app->router);
  csilk_config_free(&app->config);
  free(app);
  return NULL;
}

/** @brief Deallocate all application resources. */
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

/** @brief Set the minimum log level. */
void csilk_app_log_level(csilk_app_t* app, csilk_log_level_t lv) {
  if (!app) return;
  app->config.logger.level = lv;
  (void)csilk_log_init(app->config.logger);
}

/** @brief Enable file logging with optional rotation size. */
void csilk_app_log_file(csilk_app_t* app, const char* path, size_t max_sz) {
  if (!app) return;
  if (app->config.logger.file_path) free((void*)app->config.logger.file_path);
  app->config.logger.file_path = path ? strdup(path) : NULL;
  app->config.logger.max_file_size = max_sz;
  (void)csilk_log_init(app->config.logger);
}

/** @brief Enable or disable JSON structured log output. */
void csilk_app_log_json(csilk_app_t* app, int enable) {
  if (!app) return;
  app->config.logger.json_format = enable;
  (void)csilk_log_init(app->config.logger);
}

/* ---- middleware ---- */

/** @brief Register a global middleware handler. */
void csilk_app_use(csilk_app_t* app, csilk_handler_t h) {
  if (!app || !app->server) return;
  csilk_server_use(app->server, h);
}

/** @brief Register a middleware scoped to a URL prefix group. */
void csilk_app_use_group(csilk_app_t* app, const char* prefix,
                         csilk_handler_t h) {
  if (!app || !prefix) return;
  csilk_group_t* g = find_or_create_group(app, prefix);
  if (g) csilk_group_use(g, h);
}

/** @brief Auto-apply built-in middleware based on current configuration. */
void csilk_app_apply_config(csilk_app_t* app) {
  if (!app) return;
  if (app->config.static_files.enable && app->config.static_files.root_dir) {
    csilk_app_static(app,
                     app->config.static_files.prefix
                         ? app->config.static_files.prefix
                         : "/static",
                     app->config.static_files.root_dir);
  }
}

/* ---- OpenAPI / Swagger ---- */

/** @brief Enable or disable the built-in /openapi.json endpoint. */
void csilk_app_enable_openapi(csilk_app_t* app, int enable) {
  (void)app;
  set_openapi_router(enable ? app->router : NULL);
  CSILK_LOG_I("OpenAPI endpoint %s", enable ? "enabled" : "disabled");
}

/* ---- routes ---- */

/** @brief Register a route with a single handler. */
void csilk_app_add_route(csilk_app_t* app, const char* method, const char* path,
                         csilk_handler_t h) {
  if (!app || !method || !path || !h) return;
  csilk_group_t* g = app->root_group;
  if (!g) return;
  csilk_group_add_route(g, method, path, h);
}

/** @brief Register a route with OpenAPI metadata (input/output types). */
void csilk_app_add_route_extended(csilk_app_t* app, const char* method,
                                  const char* path, csilk_handler_t handler,
                                  const char* input_type,
                                  const char* output_type, const char* summary,
                                  const char* description) {
  if (!app || !method || !path || !handler) return;
  csilk_group_t* g = app->root_group;
  if (!g) return;
  csilk_group_add_route_extended(g, method, path, handler, input_type,
                                 output_type, summary, description);
}

/** @brief Register a route with multiple handlers (middleware chain). */
void csilk_app_add_handlers(csilk_app_t* app, const char* method,
                            const char* path, csilk_handler_t* handlers,
                            size_t n) {
  if (!app || !method || !path || !handlers || n == 0) return;
  csilk_group_t* g = app->root_group;
  if (!g) return;
  csilk_group_add_handlers(g, method, path, handlers, n);
}

/* ---- static files ---- */

/** @brief Configure static file serving from a local directory. */
void csilk_app_static(csilk_app_t* app, const char* prefix,
                      const char* root_dir) {
  if (!app || !prefix || !root_dir) return;

  uv_once(&s_app_mutex_once, init_app_mutex);
  uv_mutex_lock(&s_app_mutex);
  if (g_static_n >= CSILK_MAX_STATIC) {
    uv_mutex_unlock(&s_app_mutex);
    CSILK_LOG_E("Static route limit (%d) reached. Route dropped: %s",
                CSILK_MAX_STATIC, prefix);
    return;
  }

  int idx = g_static_n++;
  snprintf(g_static[idx].url_prefix, sizeof(g_static[idx].url_prefix), "%s",
           prefix);
  snprintf(g_static[idx].root_dir, sizeof(g_static[idx].root_dir), "%s",
           root_dir);
  uv_mutex_unlock(&s_app_mutex);

  char wild[] = "/*path";
  char idxrt[] = "/";

  csilk_group_t* g = find_or_create_group(app, prefix);
  if (!g) return;

  csilk_handler_t hs[] = {static_serve, NULL};
  csilk_group_add_handlers(g, "GET", wild, hs, 1);
  csilk_group_add_handlers(g, "GET", idxrt, hs, 1);

  CSILK_LOG_I("static: %s -> %s", prefix, root_dir);
}

/* ---- config / run / accessors ---- */

/** @brief Apply server-level configuration options. */
void csilk_app_set_server_config(csilk_app_t* app, csilk_server_config_t c) {
  if (!app || !app->server) return;
  app->config.server = c;
  csilk_server_set_config(app->server, &app->config.server);
}

/** @brief Get a copy of the current application configuration. */
csilk_config_t* csilk_app_config(csilk_app_t* app) {
  if (!app) return NULL;
  csilk_config_t* cp = malloc(sizeof(csilk_config_t));
  if (cp) memcpy(cp, &app->config, sizeof(csilk_config_t));
  return cp;
}

/** @brief Start the server and enter the event loop. */
int csilk_app_run(csilk_app_t* app, int port) {
  if (!app) return -1;
  int p = port > 0 ? port : app->config.port;
  CSILK_LOG_I("\n  csilk server listening on http://localhost:%d\n\n", p);
  return csilk_server_run(app->server, p);
}

/** @brief Get the underlying router handle. */
csilk_router_t* csilk_app_router(csilk_app_t* app) {
  return app ? app->router : NULL;
}

/** @brief Get the underlying server handle. */
csilk_server_t* csilk_app_server(csilk_app_t* app) {
  return app ? app->server : NULL;
}
