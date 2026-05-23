/**
 * csilk easy API demo — minimal example server.
 *
 * Build:  make  (the executable is built as `example_app`)
 * Run:    ./build/example_app
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csilk.h"
#include "csilk_app.h"

/* ---- handlers ---- */

static void hello(csilk_ctx_t* c) {
    csilk_string(c, 200, "Hello from csilk easy API!");
}

static void user(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    char buf[64];
    snprintf(buf, sizeof(buf), "user id: %s", id ? id : "?");
    csilk_string(c, 200, buf);
}

static void ping(csilk_ctx_t* c) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    cJSON_AddNumberToObject(obj, "time", (double)time(NULL));
    csilk_json(c, 200, obj);
}

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

static void echo(csilk_ctx_t* c) {
    const char* q = csilk_get_query(c, "msg");
    char buf[256];
    snprintf(buf, sizeof(buf), "echo: %s", q ? q : "(nothing)");
    csilk_string(c, 200, buf);
}

/* ---- SSE demo ---- */
static void sse_handler(csilk_ctx_t* c) {
    csilk_sse_init(c);
    csilk_sse_send(c, NULL, "connected");
    csilk_sse_send(c, "greeting", "{\"hello\":\"world\"}");
    csilk_sse_close(c);
}

/* ---- custom middleware ---- */
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
