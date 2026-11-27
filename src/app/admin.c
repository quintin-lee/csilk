/**
 * @file admin.c
 * @brief Unified administration and monitoring controller implementation.
 */

#include "csilk/app/admin.h"
#include "csilk/app/app.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/json/json.h"
#include "csilk/core/internal.h"
#include "csilk/core/ctx/async.h"
#include "csilk/core/sync.h"
#ifdef CSILK_HAS_DB
#include "csilk/drivers/db.h"
#endif
#ifdef CSILK_HAS_AI
#include "csilk/drivers/ai.h"
#endif
#include "csilk/util/flamegraph.h"

/* Weak fallbacks for optional sub-modules (AI, DB) */
#if defined(__GNUC__) || defined(__clang__)
#ifdef CSILK_HAS_AI
__attribute__((weak)) void
csilk_ai_get_stats(csilk_ai_stats_t* stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}
__attribute__((weak)) void
csilk_ai_register_monitor(void* c)
{
    (void)c;
}
__attribute__((weak)) void
csilk_ai_unregister_monitor(void* c)
{
    (void)c;
}
#endif
#ifdef CSILK_HAS_DB
__attribute__((weak)) void
csilk_db_get_stats(csilk_db_stats_t* stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}
#endif
#endif

/* --- Async flame-graph session state --- */

typedef struct {
    csilk_async_op_t* op;
    csilk_server_t*   server;
    _Atomic(char*)    svg;
    size_t            svg_len;
    atomic_int        cancelled;
    atomic_int        ref_count;
} admin_fg_session_t;

static csilk_mutex_t       g_admin_fg_mutex;
static csilk_once_t        g_admin_fg_once = CSILK_ONCE_INIT;
static admin_fg_session_t* g_active_fg_session = NULL;
static uint64_t            last_req_count = 0;
static uint64_t            last_check_time = 0;

static void
admin_fg_init_once(void)
{
    csilk_mutex_init(&g_admin_fg_mutex);
}

static inline void
admin_fg_init(void)
{
    csilk_once(&g_admin_fg_once, admin_fg_init_once);
}

static void
admin_fg_session_unref(admin_fg_session_t* s)
{
    if (!s) {
        return;
    }
    if (atomic_fetch_sub_explicit(&s->ref_count, 1, memory_order_acq_rel) > 1) {
        return;
    }
    char* leftover = atomic_exchange_explicit(&s->svg, NULL, memory_order_acq_rel);
    free(leftover);
    free(s);
}

static void
admin_fg_defer_cleanup(void* arg)
{
    admin_fg_session_t* s = (admin_fg_session_t*)arg;
    if (!s) {
        return;
    }
    atomic_store_explicit(&s->cancelled, 1, memory_order_release);
    char* leftover = atomic_exchange_explicit(&s->svg, NULL, memory_order_acq_rel);
    free(leftover);
    admin_fg_session_unref(s);
}

static void
admin_fg_on_server_stop(csilk_server_t* server)
{
    admin_fg_init();
    bool should_stop = false;
    csilk_mutex_lock(&g_admin_fg_mutex);
    if (g_active_fg_session && g_active_fg_session->server == server) {
        atomic_store_explicit(&g_active_fg_session->cancelled, 1, memory_order_release);
        should_stop = true;
    }
    csilk_mutex_unlock(&g_admin_fg_mutex);

    if (should_stop) {
        csilk_flamegraph_stop(NULL, NULL);
    }
}

static void
admin_fg_on_complete(csilk_ctx_t* c, void* result)
{
    admin_fg_session_t* s = (admin_fg_session_t*)result;
    if (!c) {
        return;
    }

    char*  svg = s ? atomic_exchange_explicit(&s->svg, NULL, memory_order_acq_rel) : NULL;
    size_t svg_len = s ? s->svg_len : 0;

    if (svg) {
        csilk_set_header(c, "Content-Type", "image/svg+xml");
        csilk_set_header(c, "Cache-Control", "no-cache");
        csilk_status(c, 200);
        csilk_set_response_body(c, svg, svg_len, 1);
    } else {
        csilk_json_error(c, 500, "No samples collected");
    }
    _csilk_send_response(c);
}

/** @brief Flame-graph done callback (runs on sampler thread).
 *  Verifies connection/request liveness before marshalling back via csilk_async_op_t. */
static void
admin_fg_done_cb(char* svg, size_t svg_len, void* user_data)
{
    admin_fg_session_t* s = (admin_fg_session_t*)user_data;
    if (!s) {
        free(svg);
        return;
    }

    admin_fg_init();
    csilk_mutex_lock(&g_admin_fg_mutex);
    if (g_active_fg_session == s) {
        g_active_fg_session = NULL;
    }
    csilk_mutex_unlock(&g_admin_fg_mutex);

    if (atomic_load_explicit(&s->cancelled, memory_order_acquire) ||
        !csilk_async_op_is_alive(s->op)) {
        free(svg);
        csilk_async_op_cancel(s->op);
        admin_fg_session_unref(s);
        return;
    }

    s->svg_len = svg_len;
    atomic_store_explicit(&s->svg, svg, memory_order_release);

    if (csilk_async_op_complete(s->op, s) < 0) {
        char* leftover = atomic_exchange_explicit(&s->svg, NULL, memory_order_acq_rel);
        free(leftover);
    }
    admin_fg_session_unref(s);
}

static void
admin_start_flamegraph_async(csilk_ctx_t* c, useconds_t interval_us, uint64_t duration_ms)
{
    if (!c) {
        return;
    }

    admin_fg_session_t* s = calloc(1, sizeof(admin_fg_session_t));
    if (!s) {
        csilk_json_error(c, 500, "Failed to allocate profiler session");
        return;
    }
    s->server = csilk_ctx_get_server(c);
    atomic_init(&s->svg, NULL);
    atomic_init(&s->cancelled, 0);
    atomic_init(&s->ref_count, 2); /* 1 for csilk_ctx_defer, 1 for sampler done_cb */

    s->op = csilk_async_op_begin(c, 0, admin_fg_on_complete, NULL, s);
    if (!s->op) {
        free(s);
        csilk_json_error(c, 500, "Failed to start async operation");
        return;
    }

    if (csilk_ctx_defer(c, admin_fg_defer_cleanup, s) != 0) {
        csilk_async_op_cancel(s->op);
        csilk_ctx_set_async(c, 0);
        free(s);
        csilk_json_error(c, 500, "Failed to register async cleanup");
        return;
    }

    admin_fg_init();
    csilk_mutex_lock(&g_admin_fg_mutex);
    g_active_fg_session = s;
    csilk_mutex_unlock(&g_admin_fg_mutex);

    if (csilk_flamegraph_start_async(interval_us, duration_ms, admin_fg_done_cb, s) != 0) {
        csilk_mutex_lock(&g_admin_fg_mutex);
        if (g_active_fg_session == s) {
            g_active_fg_session = NULL;
        }
        csilk_mutex_unlock(&g_admin_fg_mutex);

        atomic_store_explicit(&s->cancelled, 1, memory_order_release);
        csilk_async_op_cancel(s->op);
        admin_fg_session_unref(s);
        csilk_ctx_set_async(c, 0);
        csilk_json_error(c, 500, "Failed to start profiler");
        return;
    }
}

/* Externs from metrics.c */
extern uint64_t atomic_load_http_requests(void);
extern uint64_t atomic_load_http_duration(void);

/** @brief Helper to serve the admin UI HTML. */
static void
admin_ui_handler(csilk_ctx_t* c)
{
    const char* paths[] = {"share/csilk/admin_ui.html",
                           "../share/csilk/admin_ui.html",
                           "/usr/local/share/csilk/admin_ui.html"};

    for (int i = 0; i < 3; i++) {
        FILE* f = fopen(paths[i], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                char* buf = calloc(1, (size_t)sz + 1);
                if (buf) {
                    if (fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                        csilk_set_header(c, "Content-Type", "text/html");
                        csilk_string(c, 200, buf);
                    }
                    free(buf);
                }
            }
            fclose(f);
            return;
        }
    }

    /* Embedded fallback if template file is not on disk */
    static const char g_embedded_admin_ui_html[] =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head><title>csilk Admin Dashboard</title></head>\n"
        "<body><h1>⚡ csilk Admin & APM Control Plane</h1>\n"
        "<p>Real-Time APM & Administration</p>\n"
        "</body></html>\n";

    csilk_set_header(c, "Content-Type", "text/html");
    csilk_string(c, 200, g_embedded_admin_ui_html);
}

/** @brief Aggregates system-wide statistics into a JSON response. */
static void
admin_stats_handler(csilk_ctx_t* c)
{
    csilk_json_t* root = csilk_json_object();

    // 1. HTTP Metrics
    uint64_t total_req = csilk_metrics_get_total_requests();
    uint64_t total_dur = csilk_metrics_get_total_duration();
    double   avg_latency = total_req > 0 ? (double)total_dur / total_req / 1000.0 : 0; // ms

    csilk_json_add_number(root, "total_requests", (double)total_req);
    csilk_json_add_number(root, "avg_latency", avg_latency);

    // 2. MQ Stats
    csilk_mq_stats_t mq_stats = {0};
#ifdef CSILK_HAS_MQ
    csilk_mq_get_stats(csilk_server_get_mq(csilk_ctx_get_server(c)), &mq_stats);
#endif

    csilk_json_t* mq = csilk_json_object();
    csilk_json_add_number(mq, "published", (double)mq_stats.published_total);
    csilk_json_add_number(mq, "delivered", (double)mq_stats.delivered_total);
    csilk_json_add_number(mq, "depth", (double)mq_stats.queue_depth);
    csilk_json_add_object(root, "mq", mq);

    // 3. AI Stats
#ifdef CSILK_HAS_AI
    csilk_ai_stats_t ai_stats = {0};
    csilk_ai_get_stats(&ai_stats);
#else
    struct {
        uint64_t requests_total;
        uint64_t tokens_total;
        uint64_t errors_total;
        uint64_t duration_us_total;
    } ai_stats = {0};
#endif

    csilk_json_t* ai = csilk_json_object();
    csilk_json_add_number(ai, "requests", (double)ai_stats.requests_total);
    csilk_json_add_number(ai, "tokens", (double)ai_stats.tokens_total);
    csilk_json_add_number(ai, "errors", (double)ai_stats.errors_total);
    csilk_json_add_number(ai,
                          "avg_duration",
                          ai_stats.requests_total > 0 ? (double)ai_stats.duration_us_total /
                                                            ai_stats.requests_total / 1000.0
                                                      : 0);
    csilk_json_add_object(root, "ai", ai);

    // 4. DB Stats
#ifdef CSILK_HAS_DB
    csilk_db_stats_t db_stats = {0};
    csilk_db_get_stats(&db_stats);
#else
    struct {
        uint64_t queries_total;
        uint64_t execs_total;
        uint64_t errors_total;
        uint64_t duration_us_total;
    } db_stats = {0};
#endif

    csilk_json_t* db = csilk_json_object();
    csilk_json_add_number(db, "queries", (double)db_stats.queries_total);
    csilk_json_add_number(db, "execs", (double)db_stats.execs_total);
    csilk_json_add_number(db, "errors", (double)db_stats.errors_total);
    csilk_json_add_number(db,
                          "avg_duration",
                          db_stats.queries_total + db_stats.execs_total > 0
                              ? (double)db_stats.duration_us_total /
                                    (db_stats.queries_total + db_stats.execs_total) / 1000.0
                              : 0);
    csilk_json_add_object(root, "db", db);

    // 5. System Stats
    int active_conn = 0, pooled_conn = 0;
    csilk_server_get_stats(csilk_ctx_get_server(c), &active_conn, &pooled_conn);

    size_t arena_size = 0, arena_used = 0;
    csilk_arena_get_stats(csilk_get_arena(c), &arena_size, &arena_used);

    csilk_json_t* sys = csilk_json_object();
    csilk_json_add_number(sys, "active_connections", (double)active_conn);
    csilk_json_add_number(sys, "pooled_connections", (double)pooled_conn);
    csilk_json_add_number(sys, "arena_size_kb", (double)arena_size / 1024.0);
    csilk_json_add_number(sys, "arena_used_kb", (double)arena_used / 1024.0);
    csilk_json_add_object(root, "sys", sys);

    // 6. Security Stats
    csilk_security_stats_t sec_stats;
    csilk_security_get_stats(&sec_stats);
    csilk_json_t* sec = csilk_json_object();
    csilk_json_add_number(sec, "rate_limit_blocks", (double)sec_stats.rate_limit_blocks);
    csilk_json_add_number(sec, "csrf_violations", (double)sec_stats.csrf_violations);
    csilk_json_add_number(sec, "auth_failures", (double)sec_stats.auth_failures);
    csilk_json_add_object(root, "security", sec);

    // 7. Process Stats
    csilk_process_stats_t proc_stats;
    csilk_process_get_stats(&proc_stats);
    csilk_json_t* proc = csilk_json_object();
    csilk_json_add_number(proc, "rss_kb", (double)proc_stats.rss_bytes / 1024.0);
    csilk_json_add_number(proc, "cpu_user", proc_stats.cpu_user_time_sec);
    csilk_json_add_number(proc, "cpu_sys", proc_stats.cpu_sys_time_sec);
    csilk_json_add_object(root, "process", proc);

    char* json = csilk_json_serialize(root, NULL);
    csilk_set_header(c, "Content-Type", "application/json");
    csilk_string(c, 200, json);

    free(json);
    csilk_json_free(root);
}

/** @brief Returns the router topology for graph visualization. */
static void
admin_topology_handler(csilk_ctx_t* c)
{
    csilk_server_t* s = csilk_ctx_get_server(c);
    csilk_router_t* r = csilk_server_get_router(s);
    if (!r) {
        csilk_string(c, 404, "Router not found");
        return;
    }

    csilk_json_t* routes = csilk_router_collect_routes(r);
    csilk_json(c, 200, routes);
}

/** @brief Start flame graph profiling and return the SVG result.
 *
 * Begins non-blocking stack sampling for 5 seconds (100 Hz).  The handler
 * returns immediately; the sampler thread auto-stops after the duration
 * and dispatches the SVG response back to the owning worker via
 * csilk_dispatch.  Only one profiler session runs at a time. */
static void
admin_flamegraph_handler(csilk_ctx_t* c)
{
    if (csilk_flamegraph_is_running()) {
        csilk_json_error(c, 409, "Profiler already running — try again later");
        return;
    }

    admin_start_flamegraph_async(c, 10000, 5000);

    /* Handler returns without sending a response; the async op callback
     * will write and flush the SVG when sampling completes. */
}

/** @brief Quick CPU profile — always available, non-blocking.
 *
 * Runs a brief 100ms sample and returns collapsed stacks as JSON.
 * Useful for dashboard widgets and automated monitoring. */
static void
admin_profile_quick_handler(csilk_ctx_t* c)
{
    if (csilk_flamegraph_is_running()) {
        char                 buf[256];
        csilk_bounded_json_t j;
        csilk_bounded_json_init(&j, buf, sizeof(buf));
        csilk_bounded_json_object_open(&j);
        csilk_bounded_json_key(&j, "status");
        csilk_bounded_json_string(&j, "running");
        csilk_bounded_json_key(&j, "message");
        csilk_bounded_json_string(&j, "Profiler is already running");
        csilk_bounded_json_object_close(&j);

        if (csilk_bounded_json_overflow(&j)) {
            csilk_json_t* root = csilk_json_object();
            csilk_json_add_string(root, "status", "running");
            csilk_json_add_string(root, "message", "Profiler is already running");
            csilk_json(c, 200, root);
            return;
        }

        csilk_set_header(c, "Content-Type", "application/json");
        csilk_set_status(c, 200);
        size_t         body_len = csilk_bounded_buf_len(&j.buf);
        csilk_arena_t* arena = csilk_get_arena(c);
        char* arena_body = csilk_arena_strndup(arena, csilk_bounded_json_str(&j), body_len);
        if (arena_body) {
            csilk_set_response_body(c, arena_body, body_len, 0);
        }
        return;
    }

    /* Non-blocking: sampler auto-stops after 100 ms and dispatches result */
    admin_start_flamegraph_async(c, 5000, 100);
}

typedef struct {
    csilk_mq_t*  mq;
    csilk_ctx_t* c;
} admin_monitor_cleanup_t;

static void
admin_monitor_on_close(void* arg)
{
    admin_monitor_cleanup_t* cl = (admin_monitor_cleanup_t*)arg;
    if (cl) {
#ifdef CSILK_HAS_MQ
        if (cl->mq) {
            csilk_mq_unregister_monitor(cl->mq, cl->c);
        }
#endif
#ifdef CSILK_HAS_AI
        csilk_ai_unregister_monitor(cl->c);
#endif
        free(cl);
    }
}

/** @brief WebSocket handler that streams multiplexed events. */
static void
admin_ws_handler(csilk_ctx_t* c)
{
    csilk_ws_handshake(c);
    if (csilk_is_websocket(c)) {
        csilk_mq_t* mq = NULL;
#ifdef CSILK_HAS_MQ
        mq = csilk_server_get_mq(csilk_ctx_get_server(c));
#endif
        admin_monitor_cleanup_t* cl = malloc(sizeof(admin_monitor_cleanup_t));
        if (!cl) {
            CSILK_LOG_E("Admin Dashboard: failed to allocate WebSocket cleanup context");
            return;
        }
        cl->mq = mq;
        cl->c = c;
        csilk_ctx_defer(c, admin_monitor_on_close, cl);
#ifdef CSILK_HAS_MQ
        csilk_mq_register_monitor(mq, c);
#endif
#ifdef CSILK_HAS_AI
        // Also monitor AI engine
        csilk_ai_register_monitor(c);
#endif

        CSILK_LOG_I("Admin Dashboard connected via WebSocket");
    }
}

/**
 * @brief Register the admin dashboard routes.
 * @note This function REQUIRES authentication. Use csilk_admin_serve_secure()
 *       with a valid auth middleware. Calling this without auth will log a
 *       critical error and refuse to register routes.
 * @param[in] app      Application instance.
 * @param[in] app_path Base URL path for the admin panel (e.g., "/admin").
 * @return void
 */
void
csilk_admin_serve(csilk_app_t* app, const char* app_path)
{
    // Enforce authentication requirement for security
    CSILK_LOG_F("CRITICAL: Admin dashboard must use authentication! "
                "Use csilk_admin_serve_secure(app, path, auth_middleware) instead.");
    // Do NOT register routes without auth - prevent accidental exposure
}

/**
 * @brief Register administration routes with a secure middleware.
 *
 * This implementation creates a new route group for the admin panel, applying
 * an optional authentication middleware. This ensures that sensitive metrics
 * and real-time data are only accessible to authorized users.
 *
 * @param app             The high-level application handle.
 * @param app_path        The base URL path for the admin panel (e.g.,
 * "/admin").
 * @param auth_middleware Optional handler function for authentication.
 * @return void
 */
void
csilk_admin_serve_secure(csilk_app_t* app, const char* app_path, csilk_handler_t auth_middleware)
{
    if (!app || !app_path || !auth_middleware) {
        CSILK_LOG_E(
            "Failed to serve admin dashboard: invalid app, app_path, or missing auth_middleware");
        return;
    }

    CSILK_LOG_I("Initializing admin dashboard on path '%s'", app_path);

    /* Create a dedicated route group. This allows us to apply prefixing
      and middleware to all sub-routes (/stats, /ws) in one go. */
    csilk_group_t* group = csilk_group_new(csilk_app_router(app), app_path);
    if (!group) {
        CSILK_LOG_E("Failed to initialize admin dashboard: could not create route group "
                    "for path '%s'",
                    app_path);
        return;
    }

    /* Apply mandatory security middleware to admin dashboard */
    CSILK_LOG_I("Applied security/auth middleware to admin dashboard");
    csilk_group_use(group, auth_middleware);

    /* Register sub-handlers. Because these are registered via the group,
      their paths are automatically relative to the group's prefix. */
    csilk_GET(group, "/", admin_ui_handler);                   /* GET /admin/ */
    csilk_GET(group, "/stats", admin_stats_handler);           /* GET /admin/stats */
    csilk_GET(group, "/ws", admin_ws_handler);                 /* GET /admin/ws (Upgrade) */
    csilk_GET(group, "/topology", admin_topology_handler);     /* GET /admin/topology */
    csilk_GET(group, "/flamegraph", admin_flamegraph_handler); /* GET /admin/flamegraph */
    csilk_GET(group, "/profile", admin_profile_quick_handler); /* GET /admin/profile */

    CSILK_LOG_D("Admin routes registered relative to group prefix '%s'", app_path);

    /* Ensure any active async flame-graph sampler stops cleanly before server teardown */
    csilk_server_t* srv = csilk_app_server(app);
    if (srv) {
        csilk_server_add_hook(srv, CSILK_HOOK_SERVER_STOP, (void*)admin_fg_on_server_stop);
    }

    /* The group struct and its middleware list have been copied into the router's 
     * handler chains for each registered route. The group builder object can now be freed. */
    csilk_group_free(group);
}
