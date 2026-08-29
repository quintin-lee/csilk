/**
 * @file server_lifecycle.c
 * @brief Server lifecycle — create, configure, run, free, and accessors.
 *
 * Implements the server's public API: creation, configuration, driver
 * injection, middleware registration, and the main event loop bootstrap.
 *
 * Connection I/O, HTTP parsing, TLS, shutdown, and worker threading are
 * delegated to connection.c, http1.c, tls.c, server_shutdown.c, and
 * server_worker.c respectively, declared via srv_impl.h.
 * @copyright MIT License
 */

#include <limits.h>
#include <llhttp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32
#include <sched.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "csilk/core/hot_reload.h"
#include "csilk/reflection/reflect.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "csilk/messaging/mq.h"

/* --- Centralized Atomic Initializers --- */

/** @brief Centralized initialization for all runtime config atomic fields. */
void
_csilk_runtime_config_init(csilk_runtime_config_t* rc, const csilk_server_config_t* cfg)
{
    if (!rc) {
        return;
    }
    unsigned int idle =
        cfg && cfg->idle_timeout_ms ? cfg->idle_timeout_ms : CSILK_DEFAULT_IDLE_TIMEOUT;
    unsigned int read_t = cfg ? cfg->read_timeout_ms : 0;
    unsigned int write_t = cfg ? cfg->write_timeout_ms : 0;
    unsigned int req_t = cfg ? cfg->request_timeout_ms : 0;
    size_t body = cfg && cfg->max_body_size ? cfg->max_body_size : CSILK_DEFAULT_MAX_BODY_SIZE;
    size_t hdr = cfg && cfg->max_header_size ? cfg->max_header_size : CSILK_DEFAULT_MAX_HEADER_SIZE;
    size_t url = cfg ? cfg->max_url_size : 0;
    size_t hdr_cnt = cfg ? cfg->max_headers_count : 0;
    int    max_conn = cfg ? cfg->max_connections : 0;
    int    simd = cfg ? cfg->enable_simd : 1;
    int    h2_push = cfg ? cfg->h2_push_enable : 0;
    int    h2_push_max = cfg && cfg->h2_max_push_per_request ? cfg->h2_max_push_per_request : 10;
    size_t bp_queue = cfg ? cfg->backpressure_max_queue_depth : 0;
    unsigned int bp_lat = cfg ? cfg->backpressure_max_latency_us : 0;

    atomic_init(&rc->idle_timeout_ms, idle);
    atomic_init(&rc->read_timeout_ms, read_t);
    atomic_init(&rc->write_timeout_ms, write_t);
    atomic_init(&rc->request_timeout_ms, req_t);
    atomic_init(&rc->max_body_size, body);
    atomic_init(&rc->max_header_size, hdr);
    atomic_init(&rc->max_url_size, url);
    atomic_init(&rc->max_headers_count, hdr_cnt);
    atomic_init(&rc->max_connections, max_conn);
    atomic_init(&rc->enable_simd, simd);
    atomic_init(&rc->h2_push_enable, h2_push);
    atomic_init(&rc->h2_max_push_per_request, h2_push_max);
    atomic_init(&rc->backpressure_max_queue_depth, bp_queue);
    atomic_init(&rc->backpressure_max_latency_us, bp_lat);
}

/** @brief Centralized initialization for all server atomic fields. */
void
_csilk_server_atomics_init(csilk_server_t* s, csilk_router_t* router)
{
    if (!s) {
        return;
    }
    atomic_init(&s->router, router);
    atomic_init(&s->max_connections, 0);
    atomic_init(&s->active_connections, 0);
    for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
        atomic_init(&s->hooks[i], NULL);
    }
    _csilk_runtime_config_init(&s->runtime_config, &s->config);
    _csilk_reload_mgr_init(s);
}

/* --- Server creation --- */

/** @brief Create a new server instance associated with a router. */
csilk_server_t*
csilk_server_new(csilk_router_t* router)
{
    csilk_reflect_init();
    csilk_arena_init();
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    if (!s) {
        return NULL;
    }

    s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
    s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
    s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
    s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;

    /* Centralized initialization of all server atomics and runtime configs */
    _csilk_server_atomics_init(s, router);

#if defined(CSILK_USE_URING) && CSILK_USE_URING
    s->loop = calloc(1, sizeof(csilk_io_loop_t));
    if (!s->loop || csilk_io_loop_init(s->loop) != 0) {
        free(s->loop);
        s->loop = NULL;
        csilk_server_free(s);
        return NULL;
    }
    s->loop_owned = 1;
#else
    s->loop = csilk_io_default_loop();
    if (!s->loop) {
        csilk_server_free(s);
        return NULL;
    }
    s->loop_owned = 0;
#endif

    s->hot_reload_ctx = NULL;
    llhttp_settings_init(&s->settings);
    s->settings.on_message_begin = on_message_begin;
    s->settings.on_url = on_url;
    s->settings.on_header_field = on_header_field;
    s->settings.on_header_field_complete = on_header_field_complete;
    s->settings.on_header_value = on_header_value;
    s->settings.on_header_value_complete = on_header_value_complete;
    s->settings.on_headers_complete = on_headers_complete;
    s->settings.on_body = on_body;
    s->settings.on_message_complete = on_message_complete;

    csilk_mutex_init(&s->hook_mutex);
    csilk_mutex_init(&s->config_mutex);
    s->mq = NULL;

    return s;
}

/* --- SPA fallback --- */

/** @brief Built-in SPA (Single Page Application) fallback handler. */
static void
spa_fallback_handler(csilk_ctx_t* c)
{
    const char* method = csilk_get_method(c);
    if (!method || strcmp(method, "GET") != 0) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client || !client->server || !client->server->spa_doc_root) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/index.html", client->server->spa_doc_root);

    FILE* f = fopen(path, "rb");
    if (!f) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    rewind(f);
    char* body = malloc((size_t)fsize + 1); // NOLINT(clang-analyzer-unix.Errno)
    if (!body) {
        int saved_errno = errno;
        fclose(f);
        errno = saved_errno;
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
        return;
    }
    size_t nread = fread(body, 1, (size_t)fsize, f);
    fclose(f);
    csilk_set_header(c, "Content-Type", "text/html");
    csilk_set_response_body_ex(c, body, nread, CSILK_OWN_HEAP);
    csilk_status(c, CSILK_STATUS_OK);
}

/* --- Server configuration --- */

/** @brief Set a custom handler for unmatched routes (404 Not Found). */
void
csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler)
{
    if (!server) {
        return;
    }
    server->not_found_handler = handler;
}

/** @brief Enable SPA fallback: all unmatched GET requests serve index.html. */
void
csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root)
{
    if (!server || !doc_root) {
        return;
    }
    free(server->spa_doc_root);
    server->spa_doc_root = strdup(doc_root);
    if (server->spa_doc_root) {
        server->not_found_handler = spa_fallback_handler;
    }
}

/** @brief Register a global middleware handler that runs before every request. */
int
csilk_server_use(csilk_server_t* server, csilk_handler_t handler)
{
    if (!server || !handler) {
        return -1;
    }
    if (server->middleware_count >= 32) {
        CSILK_LOG_E("Server: global middleware limit (32) reached. Middleware "
                    "dropped.");
        return -1;
    }
    server->middlewares[server->middleware_count++] = handler;
    CSILK_LOG_D("Server: registered global middleware %p (count: %d)",
                (void*)handler,
                server->middleware_count);
    if (server->router) {
        csilk_router_compile(server->router, server->middlewares, (size_t)server->middleware_count);
    }
    return 0;
}

/* --- Server free --- */

/** @brief Free a server instance and all associated resources. */
void
csilk_server_free(csilk_server_t* server)
{
    if (!server) {
        return;
    }

    if (server->worker_tids) {
        for (int i = 0; i < server->worker_count; i++) {
            csilk_thread_join(&server->worker_tids[i]);
        }
        free(server->worker_tids);
        server->worker_tids = NULL;
    }

    if (server->worker_barrier) {
        csilk_barrier_destroy(server->worker_barrier);
        free(server->worker_barrier);
        server->worker_barrier = NULL;
    }

    if (server->mq) {
        _csilk_mq_free(server->mq);
        server->mq = NULL;
    }

    free(server->spa_doc_root);
    if (server->worker_pools) {
        for (int w = 0; w < server->worker_pool_count; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
            _csilk_worker_drain_dispatch(wp);
            if (wp->dispatch_async.type != 0 &&
                !csilk_io_is_closing((csilk_io_handle_t*)&wp->dispatch_async)) {
                csilk_io_close((csilk_io_handle_t*)&wp->dispatch_async, NULL);
                if (server->loop) {
                    csilk_io_run(server->loop, CSILK_IO_RUN_NOWAIT);
                }
            }
            int client_cnt = atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed);
            for (int i = 0; i < client_cnt; i++) {
                free(wp->client_pool[i]);
            }
            int arena_cnt = atomic_load_explicit(&wp->arena_pool_count, memory_order_relaxed);
            for (int i = 0; i < arena_cnt; i++) {
                csilk_arena_free(wp->arena_pool[i]);
            }
            for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
                int buf_cnt =
                    atomic_load_explicit(&wp->read_buf_counts[tier], memory_order_relaxed);
                for (int i = 0; i < buf_cnt; i++) {
                    free(wp->read_buf_tiers[tier][i]);
                }
            }
        }
        free(server->worker_pools);
    }

    cleanup_tls(server);

    for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
        csilk_hook_array_t* arr = atomic_load_explicit(&server->hooks[i], memory_order_relaxed);
        if (arr) {
            free(arr);
            atomic_store_explicit(&server->hooks[i], NULL, memory_order_relaxed);
        }
    }
    csilk_mutex_destroy(&server->hook_mutex);
    csilk_mutex_destroy(&server->config_mutex);

    csilk_dev_hot_reload_stop(server);
    csilk_server_wait_grace_period(server);
    _csilk_reload_mgr_free(server);
    _csilk_dispatch_pool_cleanup();

    if (server->loop_owned && server->loop) {
        csilk_io_loop_close(server->loop);
        free(server->loop);
        server->loop = NULL;
    }

    csilk_arena_flush_free_list();
    csilk_body_pool_cleanup();
    free(server);
}

/* --- Server stats --- */

/** @brief Read the server's live connection statistics. */
void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
    if (!server) {
        return;
    }
    int active_total = 0;
    int pooled_total = 0;
    int n = server->worker_pool_count;

    if (server->worker_pools && n > 0) {
        for (int w = 0; w < n; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
            pooled_total += atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed);
            active_total += atomic_load_explicit(&wp->active_connections, memory_order_relaxed);
        }
    } else {
        active_total = atomic_load_explicit(&server->active_connections, memory_order_relaxed);
    }

    if (active_conn) {
        *active_conn = active_total > 0 ? active_total : 0;
    }
    if (pooled_conn) {
        *pooled_conn = pooled_total > 0 ? pooled_total : 0;
    }
}

/* --- Server config --- */

/** @brief Apply a server configuration struct. */
void
csilk_server_set_config(csilk_server_t* server, const csilk_server_config_t* config)
{
    if (!server || !config) {
        return;
    }

    csilk_mutex_lock(&server->config_mutex);
    csilk_server_config_t old = server->config;

    server->config = *config;

    unsigned int idle =
        server->config.idle_timeout_ms
            ? server->config.idle_timeout_ms
            : (old.idle_timeout_ms ? old.idle_timeout_ms : CSILK_DEFAULT_IDLE_TIMEOUT);
    size_t body = server->config.max_body_size
                      ? server->config.max_body_size
                      : (old.max_body_size ? old.max_body_size : CSILK_DEFAULT_MAX_BODY_SIZE);
    size_t hdr = server->config.max_header_size
                     ? server->config.max_header_size
                     : (old.max_header_size ? old.max_header_size : CSILK_DEFAULT_MAX_HEADER_SIZE);
    int    backlog = server->config.listen_backlog
                         ? server->config.listen_backlog
                         : (old.listen_backlog ? old.listen_backlog : CSILK_DEFAULT_LISTEN_BACKLOG);

    server->config.idle_timeout_ms = idle;
    server->config.max_body_size = body;
    server->config.max_header_size = hdr;
    server->config.listen_backlog = backlog;

    /* Atomically update dynamic runtime configuration */
    atomic_store_explicit(
        &server->max_connections, server->config.max_connections, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.max_connections,
                          server->config.max_connections,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.idle_timeout_ms, idle, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.read_timeout_ms,
                          server->config.read_timeout_ms,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.write_timeout_ms,
                          server->config.write_timeout_ms,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.request_timeout_ms,
                          server->config.request_timeout_ms,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.max_body_size, body, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.max_header_size, hdr, memory_order_relaxed);
    atomic_store_explicit(
        &server->runtime_config.max_url_size, server->config.max_url_size, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.max_headers_count,
                          server->config.max_headers_count,
                          memory_order_relaxed);
    atomic_store_explicit(
        &server->runtime_config.enable_simd, server->config.enable_simd, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.h2_push_enable,
                          server->config.h2_push_enable,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.h2_max_push_per_request,
                          server->config.h2_max_push_per_request,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.backpressure_max_queue_depth,
                          server->config.backpressure_max_queue_depth,
                          memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.backpressure_max_latency_us,
                          server->config.backpressure_max_latency_us,
                          memory_order_relaxed);
    csilk_mutex_unlock(&server->config_mutex);
}

/**
 * @brief Report whether the server is over its backpressure connection limit.
 * @param[in] server Server to query (validated non-NULL).
 * @return 1 if active connections exceed config.backpressure_max_queue_depth
 *         (when that limit is > 0), otherwise 0.
 * @note Returns 0 for a NULL server or when backpressure limiting is disabled.
 */
int
csilk_server_check_backpressure(csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    size_t limit = atomic_load_explicit(&server->runtime_config.backpressure_max_queue_depth,
                                        memory_order_relaxed);
    if (limit > 0) {
        size_t total_active =
            (size_t)atomic_load_explicit(&server->active_connections, memory_order_relaxed);
        if (total_active > limit) {
            return 1;
        }
    }
    return 0;
}

/* --- Max connections --- */

/** @brief Set the maximum number of concurrent client connections. */
int
csilk_server_set_max_connections(csilk_server_t* server, int max)
{
    if (!server || max < 0) {
        return -1;
    }
    csilk_mutex_lock(&server->config_mutex);
    int prev = atomic_exchange_explicit(&server->max_connections, max, memory_order_relaxed);
    atomic_store_explicit(&server->runtime_config.max_connections, max, memory_order_relaxed);
    server->config.max_connections = max;
    csilk_mutex_unlock(&server->config_mutex);
    return prev;
}

/* --- Server run --- */

/**
 * @brief Handler that serves the auto-generated OpenAPI JSON document.
 * @param[in] c Request context used to resolve the server/router and respond.
 * @note Looks up the owning server and router; on success calls
 *       csilk_serve_openapi, otherwise sets HTTP 500.
 */
static void
openapi_json_handler(csilk_ctx_t* c)
{
    csilk_server_t* server = csilk_ctx_get_server(c);
    if (server && server->router) {
        csilk_serve_openapi(
            c, server->router, "csilk API", CSILK_VERSION, "Auto-generated OpenAPI documentation.");
    } else {
        csilk_set_status(c, 500);
    }
}

/**
 * @brief Handler that serves the Swagger UI documentation page.
 * @param[in] c Request context.
 */
static void
swagger_docs_handler(csilk_ctx_t* c)
{
    csilk_serve_swagger_ui(c);
}

/**
 * @brief Start and run the server event loop, binding to port.
 * @param[in] server Server instance to run (validated non-NULL).
 * @param[in] port   TCP port to bind and listen on.
 * @return 0 on a clean run, -1 on invalid args or setup failure (TLS init,
 *         async init, bind/listen, or worker pool allocation).
 * @note Optionally registers GET /openapi.json and /docs /swagger handlers,
 *       initializes TLS when enabled, sets up the stop async handle, binds the
 *       listening socket (with SO_REUSEPORT when multiple workers are configured),
 *       creates per-worker pools/arenas, spawns worker threads, and runs the loop until stop.
 */
int
csilk_server_run(csilk_server_t* server, int port)
{
    if (!server) {
        return -1;
    }

    if (server->router && server->middleware_count > 0) {
        csilk_router_compile(server->router, server->middlewares, (size_t)server->middleware_count);
    }

    if (server->config.enable_openapi && server->router) {
        static csilk_handler_t openapi_handlers[] = {openapi_json_handler, NULL};
        static csilk_handler_t docs_handlers[] = {swagger_docs_handler, NULL};
        csilk_router_add(server->router, "GET", "/openapi.json", openapi_handlers, 1);
        csilk_router_add(server->router, "GET", "/docs", docs_handlers, 1);
        csilk_router_add(server->router, "GET", "/swagger", docs_handlers, 1);
        csilk_router_add(server->router, "GET", "/swagger-ui", docs_handlers, 1);
        CSILK_LOG_I("Server: OpenAPI and Swagger UI endpoints automatically registered at "
                    "/openapi.json, /docs, /swagger");
    }

    if (server->config.enable_tls) {
        init_tls(server);
        if (!server->ssl_ctx) {
            CSILK_LOG_E("Server: failed to initialize TLS context");
            return -1;
        }
    }

    int workers = server->config.worker_threads;
    if (workers <= 0) {
        workers = 1;
    }

    int r = csilk_io_async_init(server->loop, &server->async_handle, on_stop_async);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to initialize async handle: %s", csilk_io_strerror(r));
        return -1;
    }
    server->async_handle.data = server;

    r = bind_and_listen(
        server->loop, &server->server_handle, port, server->config.listen_backlog, workers > 1, 0);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to bind and listen on port %d: %s", port, csilk_io_strerror(r));
        return -1;
    }

    server->worker_pool_count = workers;
    server->worker_pools = calloc((size_t)workers, sizeof(worker_pool_t));
    if (!server->worker_pools) {
        CSILK_LOG_E("Server: failed to allocate memory for worker pools");
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
        return -1;
    }
    for (int w = 0; w < workers; w++) {
        _csilk_worker_pool_atomics_init(&server->worker_pools[w], server, w);
    }
    server->server_handle.data = &server->worker_pools[0];

    server->worker_pools[0].loop_ptr = server->loop;
    _csilk_worker_set_current_pool(&server->worker_pools[0]);
    _csilk_worker_init_arena_pool(&server->worker_pools[0]);
    _csilk_worker_init_read_buf_pool(&server->worker_pools[0]);
    _csilk_worker_init_dispatch(&server->worker_pools[0], server->loop);

    if (server->config.tcp_keepalive > 0) {
        csilk_io_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
    }

    if (workers > 1) {
        CSILK_LOG_I("Server: spawning %d worker threads...", workers - 1);
        int nworkers = workers - 1;
        server->worker_tids = malloc((size_t)nworkers * sizeof(csilk_thread_t));
        if (!server->worker_tids) {
            CSILK_LOG_E("Server: failed to allocate memory for worker thread IDs");
            csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
            return -1;
        }

        worker_data_t* worker_datas = calloc((size_t)nworkers, sizeof(worker_data_t));
        if (!worker_datas) {
            CSILK_LOG_E("Server: failed to allocate worker data array");
            free(server->worker_tids);
            server->worker_tids = NULL;
            csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
            return -1;
        }

        csilk_barrier_t* barrier = calloc(1, sizeof(csilk_barrier_t));
        int              br = csilk_barrier_init(barrier, (unsigned int)workers);
        if (br < 0) {
            CSILK_LOG_E("Server: failed to init worker barrier: %s", csilk_io_strerror(br));
            free(barrier);
            free(worker_datas);
            free(server->worker_tids);
            server->worker_tids = NULL;
            csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
            return -1;
        }

        int spawned = 0;
        for (int i = 0; i < nworkers; i++) {
            int idx = i + 1;
            worker_datas[i].wp = &server->worker_pools[idx];
            worker_datas[i].port = port;
            worker_datas[i].barrier = barrier;
            atomic_init(&worker_datas[i].success, 0);

            int tr =
                csilk_thread_create(&server->worker_tids[spawned], worker_thread, &worker_datas[i]);
            if (tr != 0) {
                CSILK_LOG_E(
                    "Server: failed to create worker thread %d: %s", idx, csilk_io_strerror(tr));
                break;
            }
            spawned++;
        }

        server->worker_count = spawned;

        /* Compensate barrier for unspawned workers to prevent deadlock */
        for (int k = spawned; k < nworkers; k++) {
            csilk_barrier_wait(barrier);
        }

        /* Wait for all spawned workers to complete bind_and_listen */
        csilk_barrier_wait(barrier);
        server->worker_barrier = barrier;

        /* Check whether all requested workers were spawned and bound successfully */
        bool all_ok = (spawned == nworkers);
        if (all_ok) {
            for (int i = 0; i < spawned; i++) {
                if (!atomic_load_explicit(&worker_datas[i].success, memory_order_acquire)) {
                    all_ok = false;
                    break;
                }
            }
        }

        free(worker_datas);

        if (!all_ok) {
            CSILK_LOG_E("Server: worker startup failed (spawned %d of %d)", spawned, nworkers);
            /* Stop and join any spawned workers */
            for (int i = 0; i < spawned; i++) {
                int idx = i + 1;
                csilk_io_async_send(&server->worker_pools[idx].stop_async);
            }
            for (int i = 0; i < spawned; i++) {
                csilk_thread_join(&server->worker_tids[i]);
            }
            free(server->worker_tids);
            server->worker_tids = NULL;
            server->worker_count = 0;

            csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
            return -1;
        }

        CSILK_LOG_I("Server: all %d worker threads spawned and ready", spawned);
    }

    r = csilk_io_signal_init(server->loop, &server->sig_handle);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to initialize signal handle: %s", csilk_io_strerror(r));
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
        return -1;
    }
    server->sig_handle.data = server;
    r = csilk_io_signal_start(&server->sig_handle, on_signal, SIGINT);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to start SIGINT signal handler: %s", csilk_io_strerror(r));
        csilk_io_close((csilk_io_handle_t*)&server->sig_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
        return -1;
    }

    CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

    _csilk_trigger_hooks(server, NULL, CSILK_HOOK_SERVER_START);

    int ret = csilk_io_run(server->loop, CSILK_IO_RUN_DEFAULT);
    for (int i = 0; i < 16 && csilk_io_loop_alive(server->loop); i++) {
        csilk_io_run(server->loop, CSILK_IO_RUN_NOWAIT);
    }
    return ret;
}

/* --- Accessors & RCU Router Management --- */

/** @brief Get the internal message queue instance for the server. */
