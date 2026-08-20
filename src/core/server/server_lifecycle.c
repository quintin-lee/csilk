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
#include "messaging/mq_internal.h"

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
#if defined(CSILK_USE_URING) && CSILK_USE_URING
    s->loop = calloc(1, sizeof(csilk_io_loop_t));
    if (!s->loop || csilk_io_loop_init(s->loop) != 0) {
        free(s->loop);
        free(s);
        return NULL;
    }
    s->loop_owned = 1;
#else
    s->loop = csilk_io_default_loop();
    if (!s->loop) {
        free(s);
        return NULL;
    }
    s->loop_owned = 0;
#endif
    _csilk_reload_mgr_init(s);
    atomic_init(&s->router, router);
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

    s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
    s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
    s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
    s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;
    csilk_mutex_init(&s->hook_mutex);
    for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
        atomic_init(&s->hooks[i], NULL);
    }

    s->mq = _csilk_mq_new(s->loop);

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

    free(server->spa_doc_root);
    if (server->worker_pools) {
        for (int w = 0; w < server->worker_pool_count; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
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

    if (server->mq) {
        _csilk_mq_free(server->mq);
    }

    for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
        csilk_hook_array_t* arr = atomic_load_explicit(&server->hooks[i], memory_order_relaxed);
        if (arr) {
            free(arr);
            atomic_store_explicit(&server->hooks[i], NULL, memory_order_relaxed);
        }
    }
    csilk_mutex_destroy(&server->hook_mutex);

    csilk_dev_hot_reload_stop(server);
    csilk_server_wait_grace_period(server);
    _csilk_reload_mgr_free(server);
    _csilk_dispatch_pool_cleanup();

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

    csilk_server_config_t old = server->config;

    server->config = *config;
    atomic_store(&server->max_connections, server->config.max_connections);

    if (server->config.idle_timeout_ms == 0) {
        server->config.idle_timeout_ms =
            old.idle_timeout_ms ? old.idle_timeout_ms : CSILK_DEFAULT_IDLE_TIMEOUT;
    }
    if (server->config.max_body_size == 0) {
        server->config.max_body_size =
            old.max_body_size ? old.max_body_size : CSILK_DEFAULT_MAX_BODY_SIZE;
    }
    if (server->config.max_header_size == 0) {
        server->config.max_header_size =
            old.max_header_size ? old.max_header_size : CSILK_DEFAULT_MAX_HEADER_SIZE;
    }
    if (server->config.listen_backlog == 0) {
        server->config.listen_backlog =
            old.listen_backlog ? old.listen_backlog : CSILK_DEFAULT_LISTEN_BACKLOG;
    }
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
    if (server->config.backpressure_max_queue_depth > 0) {
        size_t total_active = (size_t)atomic_load(&server->active_connections);
        if (total_active > server->config.backpressure_max_queue_depth) {
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
    int prev = atomic_exchange(&server->max_connections, max);
    server->config.max_connections = max;
    return prev;
}

/* --- Driver injection --- */

/** @brief Set the pluggable storage driver. */
void
csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver)
{
    if (server) {
        server->storage_driver = driver;
    }
}

/** @brief Set the pluggable cryptographic driver. */
void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
    if (server) {
        server->crypto_driver = driver;
    }
}

/** @brief Set the pluggable cipher driver. */
void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
    if (server) {
        server->cipher_driver = driver;
    }
}

/**
 * @brief Set the server's QUIC transport implementation.
 * @param[in] server    Server whose QUIC transport is set.
 * @param[in] transport QUIC transport handle (stored opaquely; may be NULL).
 * @note No-op if server is NULL. The transport is stored as an opaque pointer.
 */
void
csilk_server_set_quic_transport(csilk_server_t* server, csilk_quic_transport_t* transport)
{
    if (server) {
        server->quic_transport = (void*)transport;
    }
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
    server->worker_pools[0].server = server;
    server->worker_pools[0].worker_index = 0;
    server->server_handle.data = &server->worker_pools[0];

    server->worker_pools[0].loop_ptr = server->loop;
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
            server->worker_pools[idx].server = server;
            server->worker_pools[idx].worker_index = idx;

            worker_datas[i].wp = &server->worker_pools[idx];
            worker_datas[i].port = port;
            worker_datas[i].barrier = barrier;
            worker_datas[i].success = 0;

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
        csilk_barrier_destroy(barrier);
        free(barrier);

        /* Check whether all requested workers were spawned and bound successfully */
        bool all_ok = (spawned == nworkers);
        if (all_ok) {
            for (int i = 0; i < spawned; i++) {
                if (!worker_datas[i].success) {
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
csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
    return server ? server->mq : NULL;
}

/** @brief Get the server's active radix-tree router atomically. */
csilk_router_t*
csilk_server_get_router(csilk_server_t* server)
{
    return server ? atomic_load_explicit(&server->router, memory_order_acquire) : NULL;
}

CSILK_INTERNAL void
_csilk_reload_mgr_init(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    atomic_init(&mgr->global_epoch, 1);
    atomic_init(&mgr->reclaim_lock, 0);
    atomic_init(&mgr->retired_count, 0);
    atomic_init(&mgr->retired_head, NULL);

    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        atomic_init(&mgr->reader_slots[i].active_epoch, 0);
        atomic_init(&mgr->reader_slots[i].owner_tid, 0);
        atomic_init(&mgr->reader_slots[i].nesting_depth, 0);
    }
}

static csilk_rcu_slot_t*
acquire_rcu_slot(csilk_reload_mgr_t* mgr)
{
    uintptr_t my_tid = (uintptr_t)pthread_self();
    if (my_tid == 0) {
        my_tid = 1;
    }

    size_t start = (size_t)(my_tid % CSILK_RELOAD_MAX_READERS);

    /* 1. Fast path: check if this thread already owns a slot */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        size_t idx = (start + i) % CSILK_RELOAD_MAX_READERS;
        if (atomic_load_explicit(&mgr->reader_slots[idx].owner_tid, memory_order_relaxed) ==
            my_tid) {
            return &mgr->reader_slots[idx];
        }
    }

    /* 2. Slow path: claim an unused slot */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        size_t    idx = (start + i) % CSILK_RELOAD_MAX_READERS;
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&mgr->reader_slots[idx].owner_tid,
                                                    &expected,
                                                    my_tid,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            return &mgr->reader_slots[idx];
        }
    }

    /* Fallback: deterministic slot */
    return &mgr->reader_slots[start];
}

csilk_router_t*
csilk_server_router_acquire(csilk_server_t* server, csilk_rcu_token_t* token)
{
    if (!server) {
        if (token) {
            token->active = 0;
            token->slot = NULL;
            token->epoch = 0;
        }
        return NULL;
    }

    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    csilk_rcu_slot_t*   slot = acquire_rcu_slot(mgr);

    uint32_t depth = atomic_fetch_add_explicit(&slot->nesting_depth, 1, memory_order_relaxed);
    uint64_t epoch = 0;
    if (depth == 0) {
        epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);
        atomic_store_explicit(&slot->active_epoch, epoch, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
    } else {
        epoch = atomic_load_explicit(&slot->active_epoch, memory_order_relaxed);
    }

    if (token) {
        token->slot = slot;
        token->epoch = epoch;
        token->active = 1;
    }

    return atomic_load_explicit(&server->router, memory_order_acquire);
}

void
csilk_server_router_release(csilk_server_t* server, csilk_rcu_token_t* token)
{
    if (!token || !token->active || !token->slot) {
        return;
    }
    (void)server;
    csilk_rcu_slot_t* slot = (csilk_rcu_slot_t*)token->slot;
    token->active = 0;
    token->slot = NULL;
    token->epoch = 0;

    uint32_t depth = atomic_fetch_sub_explicit(&slot->nesting_depth, 1, memory_order_relaxed);
    if (depth <= 1) {
        atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
    }
}

CSILK_INTERNAL void
_csilk_reload_try_reclaim(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &mgr->reclaim_lock, &expected, 1, memory_order_acquire, memory_order_relaxed)) {
        return; /* Another thread is currently performing reclamation */
    }

    uint64_t current_epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);
    uint64_t min_active_epoch = UINT64_MAX;
    bool     has_active_readers = false;

    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        uint64_t r_epoch =
            atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire);
        if (r_epoch != 0) {
            has_active_readers = true;
            if (r_epoch < min_active_epoch) {
                min_active_epoch = r_epoch;
            }
        }
    }

    if (!has_active_readers) {
        min_active_epoch = current_epoch + 2;
    }

    csilk_retired_router_t* list =
        atomic_exchange_explicit(&mgr->retired_head, NULL, memory_order_acq_rel);
    csilk_retired_router_t* retain_head = NULL;
    uint32_t                retained_count = 0;

    while (list) {
        csilk_retired_router_t* next =
            atomic_load_explicit(&list->retired_next, memory_order_relaxed);
        if (list->retired_epoch < min_active_epoch) {
            /* Safe to free: no active reader can reach this router or dynamic library */
            if (list->router) {
                csilk_router_free(list->router);
            }
            if (list->dl_handle) {
#ifndef _WIN32
                dlclose(list->dl_handle);
#else
                FreeLibrary((HMODULE)list->dl_handle);
#endif
            }
            if (list->tmp_path) {
                unlink(list->tmp_path);
                free(list->tmp_path);
            }
            free(list);
        } else {
            atomic_store_explicit(&list->retired_next, retain_head, memory_order_relaxed);
            retain_head = list;
            retained_count++;
        }
        list = next;
    }

    if (retain_head) {
        csilk_retired_router_t* tail = retain_head;
        while (atomic_load_explicit(&tail->retired_next, memory_order_relaxed) != NULL) {
            tail = atomic_load_explicit(&tail->retired_next, memory_order_relaxed);
        }
        csilk_retired_router_t* cur_retired =
            atomic_load_explicit(&mgr->retired_head, memory_order_relaxed);
        do {
            atomic_store_explicit(&tail->retired_next, cur_retired, memory_order_relaxed);
        } while (!atomic_compare_exchange_weak_explicit(&mgr->retired_head,
                                                        &cur_retired,
                                                        retain_head,
                                                        memory_order_release,
                                                        memory_order_relaxed));
    }

    atomic_store_explicit(&mgr->retired_count, retained_count, memory_order_relaxed);
    atomic_store_explicit(&mgr->reclaim_lock, 0, memory_order_release);
}

void
csilk_server_set_router_full(csilk_server_t* server,
                             csilk_router_t* router,
                             void*           dl_handle,
                             const char*     tmp_path)
{
    if (!server || !router) {
        return;
    }

    if (server->middleware_count > 0) {
        csilk_router_compile(router, server->middlewares, (size_t)server->middleware_count);
    }

    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    /* Atomically swap new router into place */
    csilk_router_t* old_router =
        atomic_exchange_explicit(&server->router, router, memory_order_acq_rel);

    /* Monotonically advance global reload epoch */
    uint64_t retired_epoch = atomic_fetch_add_explicit(&mgr->global_epoch, 1, memory_order_acq_rel);

    if (old_router || dl_handle || tmp_path) {
        csilk_retired_router_t* rec = calloc(1, sizeof(csilk_retired_router_t));
        if (rec) {
            rec->router = old_router;
            rec->dl_handle = dl_handle;
            rec->tmp_path = tmp_path ? strdup(tmp_path) : NULL;
            rec->retired_epoch = retired_epoch;
            atomic_init(&rec->retired_next, NULL);

            csilk_retired_router_t* old_head =
                atomic_load_explicit(&mgr->retired_head, memory_order_relaxed);
            do {
                atomic_store_explicit(&rec->retired_next, old_head, memory_order_relaxed);
            } while (!atomic_compare_exchange_weak_explicit(
                &mgr->retired_head, &old_head, rec, memory_order_release, memory_order_relaxed));

            atomic_fetch_add_explicit(&mgr->retired_count, 1, memory_order_relaxed);
        }
    }

    /* Opportunistic non-blocking reclamation */
    _csilk_reload_try_reclaim(server);
}

void
csilk_server_set_router_ex(csilk_server_t* server, csilk_router_t* router, void* dl_handle)
{
    csilk_server_set_router_full(server, router, dl_handle, NULL);
}

void
csilk_server_set_router(csilk_server_t* server, csilk_router_t* router)
{
    csilk_server_set_router_full(server, router, NULL, NULL);
}

void
csilk_server_wait_grace_period(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    uint64_t target_epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);

    while (1) {
        bool all_done = true;
        for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
            uint64_t r_epoch =
                atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire);
            if (r_epoch != 0 && r_epoch <= target_epoch) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            break;
        }
        sched_yield();
        usleep(1000); /* 1 ms */
    }

    _csilk_reload_try_reclaim(server);
}

CSILK_INTERNAL void
_csilk_reload_mgr_free(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    /* Wait for all readers to exit */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        while (atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire) !=
               0) {
            sched_yield();
        }
    }

    /* Reclaim all retired entries unconditionally */
    csilk_retired_router_t* list =
        atomic_exchange_explicit(&mgr->retired_head, NULL, memory_order_acq_rel);
    while (list) {
        csilk_retired_router_t* next =
            atomic_load_explicit(&list->retired_next, memory_order_relaxed);
        if (list->router) {
            csilk_router_free(list->router);
        }
        if (list->dl_handle) {
#ifndef _WIN32
            dlclose(list->dl_handle);
#else
            FreeLibrary((HMODULE)list->dl_handle);
#endif
        }
        if (list->tmp_path) {
            unlink(list->tmp_path);
            free(list->tmp_path);
        }
        free(list);
        list = next;
    }
}
