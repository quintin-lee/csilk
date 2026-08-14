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
#endif

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
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
    s->loop = uv_default_loop();
    if (!s->loop) {
        free(s);
        return NULL;
    }
    s->router = router;
    llhttp_settings_init(&s->settings);
    s->settings.on_message_begin = on_message_begin;
    s->settings.on_url = on_url;
    s->settings.on_header_field = on_header_field;
    s->settings.on_header_value = on_header_value;
    s->settings.on_headers_complete = on_headers_complete;
    s->settings.on_body = on_body;
    s->settings.on_message_complete = on_message_complete;

    s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
    s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
    s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
    s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;

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
    body[nread] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)

    csilk_set_header(c, "Content-Type", "text/html");
    c->response.body = body;
    c->response.body_len = nread;
    c->response.body_is_managed = 1;
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
            uv_thread_join(&server->worker_tids[i]);
        }
        free(server->worker_tids);
        server->worker_tids = NULL;
    }

    free(server->spa_doc_root);
    if (server->worker_pools) {
        for (int w = 0; w < server->worker_pool_count; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
            for (int i = 0; i < wp->client_pool_count; i++) {
                free(wp->client_pool[i]);
            }
            for (int i = 0; i < wp->arena_pool_count; i++) {
                csilk_arena_free(wp->arena_pool[i]);
            }
        }
        free(server->worker_pools);
    }

    cleanup_tls(server);

    if (server->mq) {
        _csilk_mq_free(server->mq);
    }

    for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
        csilk_hook_node_t* curr = server->hooks[i];
        while (curr) {
            csilk_hook_node_t* next = curr->next;
            free(curr);
            curr = next;
        }
    }

    csilk_arena_flush_free_list();
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
    if (active_conn) {
        *active_conn = atomic_load(&server->active_connections);
    }
    if (pooled_conn) {
        int total = 0;
        for (int w = 0; w < server->worker_pool_count; w++) {
            total += server->worker_pools[w].client_pool_count;
        }
        *pooled_conn = total;
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
    if (!server) {
        return -1;
    }
    int prev = server->max_connections;
    server->max_connections = max;
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
            c, server->router, "Csilk API", "1.0.0", "Auto-generated OpenAPI documentation.");
    } else {
        csilk_set_status(c, 500);
    }
}

/**
 * @brief Start and run the server event loop, binding to port.
 * @param[in] server Server instance to run (validated non-NULL).
 * @param[in] port   TCP port to bind and listen on.
 * @return 0 on a clean run, -1 on invalid args or setup failure (TLS init,
 *         async init, bind/listen, or worker pool allocation).
 * @note Optionally registers a GET /openapi.json handler, initializes TLS when
 *       enabled, sets up the stop async handle, binds the listening socket (with
 *       SO_REUSEPORT when multiple workers are configured), creates per-worker
 *       pools/arenas, spawns worker threads, and runs the libuv loop until stop.
 */
int
csilk_server_run(csilk_server_t* server, int port)
{
    if (!server) {
        return -1;
    }

    if (server->config.enable_openapi && server->router) {
        static csilk_handler_t handlers[] = {openapi_json_handler, NULL};
        csilk_router_add(server->router, "GET", "/openapi.json", handlers, 1);
        CSILK_LOG_I("Server: OpenAPI endpoint automatically registered at GET /openapi.json");
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

    int r = uv_async_init(server->loop, &server->async_handle, on_stop_async);
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
    _csilk_worker_init_dispatch(&server->worker_pools[0], server->loop);

    if (server->config.tcp_keepalive > 0) {
        uv_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
    }

    if (workers > 1) {
        CSILK_LOG_I("Server: spawning %d worker threads...", workers - 1);
        int nworkers = workers - 1;
        server->worker_tids = malloc((size_t)nworkers * sizeof(csilk_thread_t));
        if (server->worker_tids) {
            server->worker_count = nworkers;

            uv_barrier_t* barrier = calloc(1, sizeof(uv_barrier_t));
            int           br = uv_barrier_init(barrier, (unsigned int)workers);
            if (br < 0) {
                CSILK_LOG_E("Server: failed to init worker barrier: %s", csilk_io_strerror(br));
                free(barrier);
                free(server->worker_tids);
                server->worker_tids = NULL;
            } else {
                for (int i = 0; i < nworkers; i++) {
                    int idx = i + 1;
                    server->worker_pools[idx].server = server;
                    server->worker_pools[idx].worker_index = idx;

                    worker_data_t* data = malloc(sizeof(worker_data_t));
                    if (!data) {
                        CSILK_LOG_E("Server: failed to allocate memory for worker "
                                    "thread data");
                        continue;
                    }
                    data->wp = &server->worker_pools[idx];
                    data->port = port;
                    data->barrier = barrier;
                    uv_thread_create(&server->worker_tids[i], worker_thread, data);
                }

                uv_barrier_wait(barrier);
                uv_barrier_destroy(barrier);
                free(barrier);
                CSILK_LOG_I("Server: all %d worker threads spawned successfully", workers - 1);
            }
        } else {
            CSILK_LOG_E("Server: failed to allocate memory for worker thread IDs");
            free(server->worker_tids);
            server->worker_tids = NULL;
        }
    }

    r = uv_signal_init(server->loop, &server->sig_handle);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to initialize signal handle: %s", csilk_io_strerror(r));
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
        return -1;
    }
    server->sig_handle.data = server;
    r = uv_signal_start(&server->sig_handle, on_signal, SIGINT);
    if (r < 0) {
        CSILK_LOG_E("Server: failed to start SIGINT signal handler: %s", csilk_io_strerror(r));
        csilk_io_close((csilk_io_handle_t*)&server->sig_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
        return -1;
    }

    CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

    _csilk_trigger_hooks(server, NULL, CSILK_HOOK_SERVER_START);

    return csilk_io_run(server->loop, CSILK_IO_RUN_DEFAULT);
}

/* --- Accessors --- */

/** @brief Get the internal message queue instance for the server. */
csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
    return server ? server->mq : NULL;
}

/** @brief Get the server's radix-tree router. */
csilk_router_t*
csilk_server_get_router(csilk_server_t* server)
{
    return server ? server->router : NULL;
}

/** @brief Swap the router instance attached to a server. */
void
csilk_server_set_router(csilk_server_t* server, csilk_router_t* router)
{
    if (!server || !router) {
        return;
    }
    csilk_router_t* old_router = server->router;
    server->router = router;
    if (old_router) {
        csilk_router_free(old_router);
    }
}
