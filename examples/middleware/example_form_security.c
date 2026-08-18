/**
 * @file example_form_security.c
 * @brief CSRF-protected form workflow: token generation, validation, and submission.
 *
 * Demonstrates a complete CSRF-safe form submission flow:
 *   1. GET /form → generates CSRF token, stores in context + cookie
 *   2. POST /submit → validates CSRF token, processes data
 *   3. Manual verification pattern for API endpoints
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

/* ====================================================================
 *  CSRF Token Helper
 * ==================================================================== */

static void
generate_csrf_token(csilk_ctx_t* c, char* out, size_t out_len)
{
    (void)c;
    unsigned char buf[16];
    csilk_crypto_fill_random(buf, sizeof(buf));

    static const char hex[] = "0123456789abcdef";
    size_t            i = 0;
    for (i = 0; i < sizeof(buf) && i < out_len - 1; i++) {
        out[i * 2] = hex[(buf[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[buf[i] & 0xf];
    }
    out[i * 2] = '\0';
}

/* ====================================================================
 *  Handlers
 * ==================================================================== */

static void
form_handler(csilk_ctx_t* c)
{
    char token[64];
    generate_csrf_token(c, token, sizeof(token));

    /* Store token in context for validation in the submit handler */
    csilk_set_ex(c, "csrf_token", strdup(token), free);

    /* Also set as a cookie for same-value validation */
    csilk_set_cookie(c, "csrf_token", token, 3600, "/", NULL, 1, 1);

    /* Render HTML form */
    const char* html = "<!DOCTYPE html>"
                       "<html><head><title>Register</title></head>"
                       "<body>"
                       "<h2>User Registration</h2>"
                       "<form method='POST' action='/submit'>"
                       "  <label>Name: <input name='name' required></label><br/>"
                       "  <label>Email: <input name='email' type='email' required></label><br/>"
                       "  <input type='hidden' name='csrf_token' value='%s'/>"
                       "  <button type='submit'>Register</button>"
                       "</form>"
                       "<p><small>CSRF token: %s</small></p>"
                       "</body></html>";

    char body[1024];
    snprintf(body, sizeof(body), html, token, token);

    csilk_set_header(c, "Content-Type", "text/html; charset=utf-8");
    csilk_string(c, 200, body);
}

static void
submit_handler(csilk_ctx_t* c)
{
    csilk_parse_form_urlencoded(c);

    const char* form_token = csilk_get_form_field(c, "csrf_token");
    const char* stored_token = csilk_get(c, "csrf_token");
    const char* name = csilk_get_form_field(c, "name");
    const char* email = csilk_get_form_field(c, "email");

    if (!form_token || !stored_token || strcmp(form_token, stored_token) != 0) {
        csilk_json_error(c, 403, "Invalid or missing CSRF token");
        return;
    }

    if (!name || strlen(name) == 0) {
        csilk_json_error(c, 400, "name is required");
        return;
    }
    if (!email || strlen(email) == 0) {
        csilk_json_error(c, 400, "email is required");
        return;
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "id", 1);
    csilk_json_add_string(obj, "name", name);
    csilk_json_add_string(obj, "email", email);
    csilk_json_add_bool(obj, "csrf_validated", true);
    csilk_json_add_bool(obj, "registered", true);
    csilk_json(c, 201, obj);
}

static void
api_submit_handler(csilk_ctx_t* c)
{
    /* JSON API reads CSRF token from header */
    const char* api_token = csilk_get_header(c, "X-CSRF-Token");
    const char* stored = csilk_get(c, "csrf_token");

    if (!api_token || !stored || strcmp(api_token, stored) != 0) {
        csilk_json_error(c, 403, "X-CSRF-Token header missing or invalid");
        return;
    }

    csilk_json_t* body = csilk_bind_json(c);
    if (!body) {
        csilk_json_error(c, 400, "invalid json");
        return;
    }

    const char* action = csilk_json_get_string(body, "action");
    if (!action) {
        csilk_json_error(c, 400, "action is required");
        return;
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "action", action);
    csilk_json_add_bool(obj, "csrf_validated", true);
    csilk_json_add_bool(obj, "processed", true);
    csilk_json(c, 200, obj);
}

static void
refresh_token_handler(csilk_ctx_t* c)
{
    char token[64];
    generate_csrf_token(c, token, sizeof(token));
    csilk_set_ex(c, "csrf_token", strdup(token), free);
    csilk_set_cookie(c, "csrf_token", token, 3600, "/", NULL, 1, 1);

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "csrf_token", token);
    csilk_json_add_string(obj, "message", "token refreshed");
    csilk_json(c, 200, obj);
}

static void
health_handler(csilk_ctx_t* c)
{
    (void)c;
    csilk_json_string(c, 200, "{\"status\":\"ok\"}");
}

/* ====================================================================
 *  Main
 * ==================================================================== */

int
main(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    /* Middleware stack */
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);

    /* Form-based CSRF flow */
    csilk_GET(root, "/form", form_handler);
    csilk_POST(root, "/submit", submit_handler);

    /* JSON API with header-based CSRF */
    csilk_POST(root, "/api/submit", api_submit_handler);

    /* Token refresh */
    csilk_GET(root, "/csrf/refresh", refresh_token_handler);

    /* Health check */
    csilk_GET(root, "/health", health_handler);

    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("\n=== CSRF Security Example ===\n");
    printf("GET  /form          — Render registration form with CSRF token\n");
    printf("POST /submit        — Submit form (validates hidden field CSRF token)\n");
    printf("POST /api/submit    — JSON API (validates X-CSRF-Token header)\n");
    printf("GET  /csrf/refresh  — Rotate CSRF token\n");
    printf("\nAttack demo (missing token → 403):\n");
    printf("  curl -X POST http://localhost:8080/submit \\\n");
    printf("       -d 'name=Alice&email=a@b.com'\n");
    printf("\nNormal flow:\n");
    printf("  1. GET /form       → get token from response body/cookie\n");
    printf("  2. POST /submit    → include token in form field\n");
    printf("Listen: http://localhost:8080\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
