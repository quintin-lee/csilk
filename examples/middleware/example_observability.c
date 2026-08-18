/**
 * @file example_observability.c
 * @brief Request ID propagation, custom cache middleware, and server hooks.
 *
 * Demonstrates observability patterns: per-request tracing IDs, a lightweight
 * in-memory cache middleware, and hook-based lifecycle monitoring.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/core/hooks.h"
#include "csilk/core/middleware.h"

/* ====================================================================
 *  Simple in-memory cache for demonstration
 * ==================================================================== */

#define CACHE_SIZE 64

typedef struct {
    char   key[128];
    char   value[4096];
    time_t expires_at;
    int    active;
} cache_entry_t;

static cache_entry_t g_cache[CACHE_SIZE];
static int           g_cache_hits = 0;
static int           g_cache_misses = 0;

static const char*
cache_lookup(const char* key)
{
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!g_cache[i].active) {
            continue;
        }
        if (strcmp(g_cache[i].key, key) != 0) {
            continue;
        }
        if (now > g_cache[i].expires_at) {
            g_cache[i].active = 0;
            continue;
        }
        g_cache_hits++;
        return g_cache[i].value;
    }
    g_cache_misses++;
    return NULL;
}

static void
cache_store(const char* key, const char* value, int ttl_sec)
{
    time_t now = time(NULL);
    int    slot = -1;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!g_cache[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < CACHE_SIZE; i++) {
            if (g_cache[i].expires_at < g_cache[slot].expires_at) {
                slot = i;
            }
        }
    }

    cache_entry_t* e = &g_cache[slot];
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
    e->expires_at = now + ttl_sec;
    e->active = 1;
}

/* ====================================================================
 *  Custom cache middleware
 * ==================================================================== */

static void
cache_middleware(csilk_ctx_t* c)
{
    /* Only cache GET requests without bypass header */
    const char* bypass = csilk_get_header(c, "X-Cache-Control");
    if (bypass && strcmp(bypass, "no-cache") == 0) {
        csilk_next(c);
        return;
    }

    const char* path = csilk_get_path(c);
    const char* cached = cache_lookup(path);
    if (cached) {
        csilk_set_header(c, "X-Cache", "HIT");
        csilk_string(c, 200, cached);
        return;
    }

    /* Cache miss — run normal chain, then store result */
    csilk_next(c);

    if (csilk_get_status(c) == 200) {
        size_t      body_len = 0;
        const char* body = csilk_get_body(c, &body_len);
        if (body && body_len > 0) {
            cache_store(path, body, 60);
            csilk_set_header(c, "X-Cache", "MISS");
        }
    }
}

/* ====================================================================
 *  Hook callbacks — must match the registered type signature
 * ==================================================================== */

static void
on_request_begin_hook(csilk_ctx_t* c)
{
    const char* method = csilk_get_method(c);
    const char* path = csilk_get_path(c);
    const char* rid = csilk_get_request_id(c);
    printf("[hook:request_begin] %s %s (id=%s)\n",
           method ? method : "?",
           path ? path : "?",
           rid ? rid : "?");
}

static void
on_request_end_hook(csilk_ctx_t* c)
{
    (void)c; /* status and body_len accessible via getters if needed */
    printf("[hook:request_end]\n");
}

/* ====================================================================
 *  Handlers
 * ==================================================================== */

static void
slow_query_handler(csilk_ctx_t* c)
{
    (void)c;
    usleep(50000); /* 50ms delay */
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "result", 42);
    csilk_json_add_int(obj, "timestamp", (int64_t)time(NULL));
    csilk_json(c, 200, obj);
}

static void
dynamic_handler(csilk_ctx_t* c)
{
    const char* name = csilk_get_param(c, "name");
    if (!name) {
        name = "world";
    }

    /* Store request ID in context for downstream use */
    const char* rid = csilk_get_request_id(c);
    if (rid) {
        csilk_set_ex(c, "request_idStored", (void*)rid, NULL);
    }

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "hello", name);
    csilk_json_add_int(obj, "ts", (int64_t)time(NULL));
    csilk_json(c, 200, obj);
}

static void
cache_stats_handler(csilk_ctx_t* c)
{
    (void)c;
    int    total = g_cache_hits + g_cache_misses;
    double hit_rate = total > 0 ? (double)g_cache_hits * 100 / total : 0.0;

    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_int(obj, "hits", (int64_t)g_cache_hits);
    csilk_json_add_int(obj, "misses", (int64_t)g_cache_misses);
    csilk_json_add_number(obj, "hit_rate", hit_rate);
    csilk_json(c, 200, obj);
}

static void
obs_health_handler(csilk_ctx_t* c)
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

    /* Core middleware */
    csilk_group_use(root, csilk_recovery_handler);
    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_request_id_middleware);

    /* Custom cache middleware */
    csilk_group_use(root, cache_middleware);

    /* Routes */
    csilk_GET(root, "/slow-query", slow_query_handler);
    csilk_GET(root, "/hello/:name", dynamic_handler);
    csilk_GET(root, "/cache/stats", cache_stats_handler);
    csilk_GET(root, "/health", obs_health_handler);

    /* Server hooks for observability */
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    csilk_server_add_hook(server, CSILK_HOOK_REQUEST_BEGIN, on_request_begin_hook);
    csilk_server_add_hook(server, CSILK_HOOK_REQUEST_END, on_request_end_hook);

    printf("\n=== Observability Example ===\n");
    printf("GET /slow-query       — Cached query (try twice to see HIT)\n");
    printf("GET /hello/:name      — With request ID storage\n");
    printf("GET /cache/stats      — Cache hit/miss statistics\n");
    printf("GET /health           — Health check\n");
    printf("\nBypass cache:\n");
    printf("  curl -H 'X-Cache-Control: no-cache' http://localhost:8080/slow-query\n");
    printf("\nWatch hooks in stdout:\n");
    printf("  [hook:request_begin] GET /slow-query (id=xxx-xxx)\n");
    printf("  [hook:request_end]\n");
    printf("Listen: http://localhost:8080\n");

    csilk_server_run(server, 8080);

    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);
    return 0;
}
