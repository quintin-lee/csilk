#include "csilk_internal.h"
/**
 * @file example_app.c
 * @brief Minimal example using the high-level csilk_app_t API.
 *
 * Demonstrates csilk_app_new, csilk_app_use, csilk_app_get, csilk_app_post,
 * csilk_json, csilk_sse, and csilk_app_static APIs.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csilk.h"
#include "csilk_app.h"

/* ---- handlers ---- */

/** @brief Handler for GET / — "Hello from csilk easy API!" */
static void hello(csilk_ctx_t* c) {
    csilk_string(c, 200, "Hello from csilk easy API!");
}

/** @brief Handler for GET /user/:id — returns the user ID from path param. */
static void user(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    char buf[64];
    snprintf(buf, sizeof(buf), "user id: %s", id ? id : "?");
    csilk_string(c, 200, buf);
}

/** @brief Handler for GET /ping — returns a JSON status response. */
static void ping(csilk_ctx_t* c) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    cJSON_AddNumberToObject(obj, "time", (double)time(NULL));
    csilk_json(c, 200, obj);
}

/** @brief Handler for POST /login — authenticates user and returns a token. */
static void login(csilk_ctx_t* c) {
    cJSON* in = csilk_bind_json(c);
    if (!in) { csilk_string(c, 400, "bad json"); return; }
    cJSON* u = cJSON_GetObjectItem(in, "user");
    if (cJSON_IsString(u) && !strcmp(u->valuestring, "admin")) {
        cJSON* out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "token", "demo-token");
        csilk_json(c, 200, out);
    } else {
        csilk_string(c, 401, "unauthorized");
    }
    cJSON_Delete(in);
}

/** @brief Handler for GET /echo?msg=... — echoes back the query parameter. */
static void echo(csilk_ctx_t* c) {
    const char* q = csilk_get_query(c, "msg");
    char buf[256];
    snprintf(buf, sizeof(buf), "echo: %s", q ? q : "(nothing)");
    csilk_string(c, 200, buf);
}

/* ---- SSE demo ---- */

/** @brief Handler for GET /stream — SSE connection demo. */
static void sse_handler(csilk_ctx_t* c) {
    csilk_sse_init(c);
    csilk_sse_send(c, NULL, "connected");
    csilk_sse_send(c, "greeting", "{\"hello\":\"world\"}");
    csilk_sse_close(c);
}

/* ---- custom middleware ---- */

/** @brief Custom middleware — measures and logs request processing time. */
static void timer_mw(csilk_ctx_t* c) {
    clock_t t0 = clock();
    csilk_next(c);
    printf("  req %s %s → %d  (%.2f ms)\n",
           c->request.method, c->request.path,
           c->response.status,
           1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
}

/* ================================================================= */

int main(void) {
    csilk_app_t* app = csilk_app_new(NULL);

    csilk_app_log_level(app, CSILK_LOG_DEBUG);

    /* enable JSON structured logging (comment out for plain text) */
    /* csilk_app_log_json(app, 1); */

    /* global middleware */
    csilk_app_use(app, timer_mw);

    /* regular routes */
    csilk_app_get(app,  "/",             hello);
    csilk_app_get(app,  "/user/:id",     user);
    csilk_app_get(app,  "/ping",         ping);
    csilk_app_post(app, "/login",        login);
    csilk_app_get(app,  "/echo",         echo);
    csilk_app_get(app,  "/stream",       sse_handler);

    /* group-level middleware */
    csilk_app_use_group(app, "/api", csilk_gzip_middleware);
    csilk_app_get(app, "/api/ping", ping);   /* inherits gzip */

    /* static files (needs a dir to exist) */
    /* csilk_app_static(app, "/public", "./static"); */

    printf("\n  endpoints:\n"
           "    GET  /             hello\n"
           "    GET  /user/:id      path params\n"
           "    GET  /ping          json\n"
           "    POST /login          {\"user\":\"admin\"}\n"
           "    GET  /echo?msg=...   query params\n"
           "    GET  /stream         SSE demo\n"
           "    GET  /api/ping       gzip-compressed json\n"
           "\n");

    int rc = csilk_app_run(app, 8080);
    csilk_app_free(app);
    return rc;
}
