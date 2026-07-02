#include "scaffold.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void
ensure_dir(const char* path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void
write_cmakelists(const project_config_t* cfg, const char* base_path)
{
    char path[512];
    FILE* f;
    snprintf(path, sizeof(path), "%s/CMakeLists.txt", base_path);
    f = fopen(path, "w");
    if (cfg->local_csilk_path[0]) {
        fprintf(f,
            CMAKE_TMPL_LOCAL,
            cfg->name,
            cfg->local_csilk_path,
            cfg->name,
            cfg->name,
            cfg->name,
            cfg->local_csilk_path,
            cfg->local_csilk_path);
    } else {
        fprintf(f, CMAKE_TMPL_FC, cfg->name, cfg->name, cfg->name, cfg->name, cfg->name);
    }
    fclose(f);
}

static void
write_config(const project_config_t* cfg, const char* base_path)
{
    char path[512];
    FILE* f;
    snprintf(path, sizeof(path), "%s/config.yaml", base_path);
    f = fopen(path, "w");
    fprintf(f,
        CONFIG_TMPL,
        cfg->port,
        cfg->has_recovery,
        cfg->has_logger,
        cfg->has_gzip,
        cfg->has_waf,
        cfg->has_ratelimit);
    fclose(f);
}

static void
add_handlers(const project_config_t* cfg, char* buf, size_t sz)
{
    if (cfg->template_type == 0) {
        strncat(buf,
            "static void hello(csilk_ctx_t* c) {\n"
            "    csilk_string(c, 200, \"Hello World\");\n"
            "}\n\n",
            sz - strlen(buf) - 1);
    } else {
        strncat(buf,
            "void health_handler(csilk_ctx_t* c) {\n"
            "    csilk_json_error(c, 200, \"ok\");\n"
            "}\n\n",
            sz - strlen(buf) - 1);
    }

    if (cfg->has_ws) {
        strncat(buf,
            "void on_ws_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, "
            "int opcode) {\n"
            "    csilk_ws_send(c, payload, len, opcode);\n"
            "}\n\n"
            "static void ws_handler(csilk_ctx_t* c) {\n"
            "    csilk_set_on_ws_message(c, on_ws_message);\n"
            "    csilk_ws_handshake(c);\n"
            "}\n\n",
            sz - strlen(buf) - 1);
    }

    if (cfg->has_cors) {
        strncat(buf,
            "static void cors_mw(csilk_ctx_t* c) {\n"
            "    static const csilk_cors_config_t cfg = {\n"
            "        .allow_origin = \"*\",\n"
            "        .allow_methods = \"GET, POST, PUT, DELETE, OPTIONS\",\n"
            "        .allow_headers = \"Content-Type, Authorization\",\n"
            "        .allow_credentials = 1,\n"
            "        .max_age = 86400,\n"
            "    };\n"
            "    csilk_cors_middleware(c, &cfg);\n"
            "}\n\n",
            sz - strlen(buf) - 1);
    }
}

static void
add_routes_lowlevel(const project_config_t* cfg, char* buf, size_t sz)
{
    if (cfg->template_type == 0) {
        strncat(buf,
            "    csilk_router_add(r, \"GET\", \"/\","
            " (csilk_handler_t[]){hello}, 1);\n",
            sz - strlen(buf) - 1);
    } else {
        strncat(buf,
            "    csilk_group_t* api = csilk_group_new(r, \"/api\");\n",
            sz - strlen(buf) - 1);
        if (cfg->has_logger) {
            strncat(buf,
                "    csilk_group_use(api, csilk_logger_handler);\n",
                sz - strlen(buf) - 1);
        }
        if (cfg->has_recovery) {
            strncat(buf,
                "    csilk_group_use(api, csilk_recovery_handler);\n",
                sz - strlen(buf) - 1);
        }
        if (cfg->has_request_id) {
            strncat(buf,
                "    csilk_group_use(api, csilk_request_id_middleware);\n",
                sz - strlen(buf) - 1);
        }
        if (cfg->has_waf) {
            strncat(buf,
                "    csilk_group_use(api, csilk_waf_middleware);\n",
                sz - strlen(buf) - 1);
        }
        if (cfg->has_gzip) {
            strncat(buf,
                "    csilk_group_use(api, csilk_gzip_middleware);\n",
                sz - strlen(buf) - 1);
        }
        strncat(buf,
            "    csilk_GET(api, \"/health\", health_handler);\n",
            sz - strlen(buf) - 1);
    }

    if (cfg->has_ws) {
        strncat(buf,
            "    csilk_router_add(r, \"GET\", \"/ws\","
            " (csilk_handler_t[]){ws_handler}, 1);\n",
            sz - strlen(buf) - 1);
    }
}

static void
add_routes_app(const project_config_t* cfg, char* buf, size_t sz)
{
    if (cfg->has_recovery) {
        strncat(
            buf, "    csilk_app_use(app, csilk_recovery_handler);\n", sz - strlen(buf) - 1);
    }
    if (cfg->has_logger) {
        strncat(
            buf, "    csilk_app_use(app, csilk_logger_handler);\n", sz - strlen(buf) - 1);
    }
    if (cfg->has_request_id) {
        strncat(buf,
            "    csilk_app_use(app, csilk_request_id_middleware);\n",
            sz - strlen(buf) - 1);
    }
    if (cfg->has_waf) {
        strncat(
            buf, "    csilk_app_use(app, csilk_waf_middleware);\n", sz - strlen(buf) - 1);
    }
    if (cfg->has_gzip) {
        strncat(
            buf, "    csilk_app_use(app, csilk_gzip_middleware);\n", sz - strlen(buf) - 1);
    }
    if (cfg->has_cors) {
        strncat(buf, "    csilk_app_use(app, cors_mw);\n", sz - strlen(buf) - 1);
    }

    if (cfg->template_type == 0) {
        strncat(buf, "    csilk_app_get(app, \"/\", hello);\n", sz - strlen(buf) - 1);
    } else {
        strncat(buf,
            "    csilk_app_get(app, \"/api/health\", health_handler);\n",
            sz - strlen(buf) - 1);
    }

    if (cfg->has_ws) {
        strncat(
            buf, "    csilk_app_get(app, \"/ws\", ws_handler);\n", sz - strlen(buf) - 1);
    }
}

static void
add_app_features(const project_config_t* cfg, char* buf, size_t sz)
{
    if (cfg->has_admin) {
        strncat(buf, "    csilk_admin_serve(app, \"/admin\");\n", sz - strlen(buf) - 1);
    }
    if (cfg->has_prometheus) {
        strncat(buf,
            "    csilk_app_get(app, \"/metrics\", csilk_metrics_handler);\n",
            sz - strlen(buf) - 1);
    }
    if (cfg->has_swagger) {
        strncat(buf, "    csilk_app_enable_openapi(app, 1);\n", sz - strlen(buf) - 1);
    }
}

void
generate_project(const project_config_t* cfg, const char* base_path)
{
    char path[512];
    FILE* f;

    ensure_dir(base_path);

    write_cmakelists(cfg, base_path);
    write_config(cfg, base_path);

    // 3. src/main.c
    snprintf(path, sizeof(path), "%s/src", base_path);
    ensure_dir(path);

    snprintf(path, sizeof(path), "%s/src/main.c", base_path);
    f = fopen(path, "w");

    char handlers[4096] = "";
    char inits[1024] = "";
    char routes[4096] = "";
    char app_features[2048] = "";

    if (cfg->has_sqlite || cfg->has_mysql) {
        strncat(inits, "    csilk_db_init();\n", sizeof(inits) - strlen(inits) - 1);
    }

    add_handlers(cfg, handlers, sizeof(handlers));

    int use_app_api = (cfg->has_admin || cfg->has_prometheus || cfg->has_swagger);

    if (use_app_api) {
        add_routes_app(cfg, routes, sizeof(routes));
        add_app_features(cfg, app_features, sizeof(app_features));
        fprintf(f, MAIN_C_APP, handlers, inits, routes, app_features, cfg->port);
    } else {
        add_routes_lowlevel(cfg, routes, sizeof(routes));
        fprintf(f,
            MAIN_C_LL,
            handlers,
            inits,
            routes,
            "    csilk_server_set_spa_fallback(s, \"./public\");\n",
            cfg->port);
    }

    fclose(f);

    // 4. public/.gitkeep
    snprintf(path, sizeof(path), "%s/public", base_path);
    ensure_dir(path);
    snprintf(path, sizeof(path), "%s/public/.gitkeep", base_path);
    f = fopen(path, "w");
    fclose(f);
}
