/**
 * @file example_security_stack.c
 * @brief Comprehensive security middleware stack: CORS + JWT + CSRF + Rate Limit.
 *
 * Demonstrates composing multiple security middlewares in the correct order,
 * with per-route policy configuration and custom error responses.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

/* ====================================================================
 *  CORS Configuration — single-origin policy for demonstration
 * ==================================================================== */

static const csilk_cors_config_t cors_config = {
    .allow_origin = "https://app.example.com",
    .allow_methods = "GET, POST, PUT, DELETE, OPTIONS",
    .allow_headers = "Authorization, Content-Type, X-CSRF-Token",
    .allow_credentials = 1,
    .max_age = 3600,
};

/* ====================================================================
 *  Middleware wrappers — group_use only accepts 2 args, so wrap
 *  middleware that takes extra context via a static closure.
 * ==================================================================== */

static void
cors_rate_limit_mw(csilk_ctx_t* c)
{
    csilk_cors_middleware(c, &cors_config);
    csilk_rate_limit_middleware(c, 60); /* 60 req/min */
}

static void
api_cors_rate_limit_mw(csilk_ctx_t* c)
{
    csilk_cors_middleware(c, &cors_config);
    csilk_rate_limit_middleware(c, 30); /* 30 req/min */
}

static void
api_jwt_mw(csilk_ctx_t* c)
{
    csilk_jwt_middleware(c, "my-secret-key-for-jwt-signing");
}

static void
csrf_mw(csilk_ctx_t* c)
{
    csilk_csrf_middleware(c);
}

/* ====================================================================
 *  Route Handlers
 * ==================================================================== */

static void
public_handler(csilk_ctx_t* c)
{
    csilk_json_string(c, 200, "{\"message\":\"public\",\"status\":\"ok\"}");
}

static void
login_handler(csilk_ctx_t* c)
{
    csilk_json_t* body = csilk_bind_json(c);
    if (!body) {
        csilk_json_error(c, 400, "invalid json body");
        return;
    }

    const char* username = csilk_json_get_string(body, "username");
    const char* password = csilk_json_get_string(body, "password");

    if (!username || !password || strlen(username) < 3 || strlen(password) < 6) {
        csilk_json_error(c, 400, "invalid credentials");
        return;
    }

    char token[256];
    snprintf(token, sizeof(token), "jwt_token_for_%s", username);

    /* Set a session cookie — 8-arg form */
    csilk_set_cookie(c, "session", token, 3600, "/", NULL, 1, 1);

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "token", token);
    csilk_json_add_int(obj, "expires_in", 3600);
    csilk_json_add_string(obj, "username", username);
    csilk_json(c, 200, obj);
}

static void
protected_resource_handler(csilk_ctx_t* c)
{
    const char* jwt_payload = csilk_get(c, "jwt_payload");
    const char* request_id = csilk_get_header(c, "X-Request-Id");
    (void)jwt_payload;

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "resource", "secret-data");
    csilk_json_add_string(obj, "request_id", request_id ? request_id : "unknown");
    csilk_json_add_bool(obj, "authenticated", true);
    csilk_json(c, 200, obj);
}

static void
create_item_handler(csilk_ctx_t* c)
{
    csilk_json_t* body = csilk_bind_json(c);
    if (!body) {
        csilk_json_error(c, 400, "invalid json body");
        return;
    }

    const char* name = csilk_json_get_string(body, "name");
    if (!name || strlen(name) == 0) {
        csilk_json_error(c, 400, "name is required");
        return;
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "id", 1);
    csilk_json_add_string(obj, "name", name);
    csilk_json_add_bool(obj, "created", true);
    csilk_json(c, 201, obj);
}

static void
delete_item_handler(csilk_ctx_t* c)
{
    const char* id_str = csilk_get_param(c, "id");
    if (!id_str) {
        csilk_json_error(c, 400, "id param required");
        return;
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "id", id_str);
    csilk_json_add_bool(obj, "deleted", true);
    csilk_json(c, 200, obj);
}

/* ====================================================================
 *  Main
 * ==================================================================== */

int
main(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    /*
     * Middleware order matters!
     *
     * 1. Recovery  — catches panics, must be first
     * 2. Logger    — logs every request
     * 3. RequestID — generates unique ID for tracing across services
     * 4. Gzip      — compresses responses (works best near bottom)
     */
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_request_id_middleware);
    csilk_group_use(root, csilk_gzip_middleware);

    /* ---- Public routes (no JWT/CSRF) ---- */
    csilk_group_t* public = csilk_group_new(router, "/public");
    csilk_group_use(public, cors_rate_limit_mw);
    csilk_GET(public, "/health", public_handler);
    csilk_POST(public, "/login", login_handler);

    /* ---- Protected API routes (JWT required) ---- */
    csilk_group_t* api = csilk_group_new(router, "/api");
    csilk_group_use(api, api_cors_rate_limit_mw);
    csilk_group_use(api, api_jwt_mw);

    /* Read endpoints — JWT only, no CSRF (idempotent) */
    csilk_GET(api, "/resource", protected_resource_handler);
    csilk_GET(api, "/items/:id", protected_resource_handler);

    /* Write endpoints — JWT + CSRF (state-changing) */
    csilk_group_t* write = csilk_group_new(router, "/api/items");
    csilk_group_use(write, csrf_mw);
    csilk_POST(write, "", create_item_handler);
    csilk_PUT(write, "/:id", create_item_handler);
    csilk_DELETE(write, "/:id", delete_item_handler);

    /* ---- Server setup ---- */
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("\n=== Security Stack Example ===\n");
    printf("Public:      GET  /public/health\n");
    printf("             POST /public/login  (json: {username, password})\n");
    printf("Protected:   GET  /api/resource  (Bearer token required)\n");
    printf("             POST /api/items      (Bearer + CSRF token required)\n");
    printf("             DELETE /api/items/:id (Bearer + CSRF token required)\n");
    printf("Listen:      http://localhost:8080\n\n");
    printf("Test login:\n");
    printf("  curl -X POST http://localhost:8080/public/login \\\n");
    printf("       -H 'Content-Type: application/json' \\\n");
    printf("       -d '{\"username\":\"admin\",\"password\":\"secret123\"}'\n\n");
    printf("Test protected:\n");
    printf("  curl http://localhost:8080/api/resource \\\n");
    printf("       -H 'Authorization: Bearer <token>'\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
