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
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "csilk/core/server/hot_reload.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Teardown Loop-Drain Bound --- */

/**
 * @brief Maximum one-shot loop iterations while draining leftover handles.
 *
 * `csilk_server_free()` closes handles (dispatch async, event loop) whose
 * close callbacks only run on the next loop iteration. Each drain iteration
 * runs `CSILK_IO_RUN_NOWAIT`, so this is a bounded anti-spin guard: it gives
 * pending close and async callbacks enough passes to fly while guaranteeing
 * teardown cannot hang forever on a stuck handle.
 *
 * All drain sites in this file MUST use this single value so the bounded
 * budget stays consistent (previously 64 vs 50 at different sites).
 */
enum { SERVER_FREE_LOOP_DRAIN_MAX = 64 };

/** @brief Maximum size of pre-read SPA index.html cached in memory (4 MiB). */
enum { CSILK_SPA_MAX_CACHE_BYTES = 4 * 1024 * 1024 };

/** @brief Async threadpool payload for SPA index.html cache-miss fallback. */
typedef struct {
    csilk_async_token_t token;
    char                doc_root[PATH_MAX];
    bool                allow_sendfile;
    int                 status_code;
    int                 fd;
    size_t              total_size;
    char*               body;
    size_t              body_len;
} spa_work_data_t;

/**
 * @brief Pump an event loop nowait up to SERVER_FREE_LOOP_DRAIN_MAX times.
 *
 * Drives pending close and async callbacks during teardown. Stops early
 * once the loop reports no more live handles. Safe to call with a NULL
 * loop.
 *
 * @param loop The event loop to drain, may be NULL.
 */
static void
_server_free_drain_loop(csilk_io_loop_t* loop)
{
    if (!loop) {
        return;
    }
    for (int i = 0; i < SERVER_FREE_LOOP_DRAIN_MAX && csilk_io_loop_alive(loop); i++) {
        csilk_io_run(loop, CSILK_IO_RUN_NOWAIT);
    }
}

CSILK_INTERNAL void
_csilk_server_drain_loop(csilk_io_loop_t* loop)
{
    _server_free_drain_loop(loop);
}

CSILK_INTERNAL void
_csilk_server_register_teardown(csilk_server_t* server, csilk_server_teardown_fn cb)
{
    if (!server || !cb) {
        return;
    }
    csilk_mutex_lock(&server->config_mutex);
    for (size_t i = 0; i < server->teardown_cb_count; i++) {
        if (server->teardown_cbs[i] == cb) {
            csilk_mutex_unlock(&server->config_mutex);
            return;
        }
    }
    if (server->teardown_cb_count <
        sizeof(server->teardown_cbs) / sizeof(server->teardown_cbs[0])) {
        server->teardown_cbs[server->teardown_cb_count++] = cb;
    }
    csilk_mutex_unlock(&server->config_mutex);
}

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
    size_t url = cfg && cfg->max_url_size ? cfg->max_url_size : CSILK_DEFAULT_MAX_URL_SIZE;
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
    csilk_arena_init();
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    if (!s) {
        return NULL;
    }

    s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
    s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
    s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
    s->config.max_url_size = CSILK_DEFAULT_MAX_URL_SIZE;
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

static bool
spa_path_is_safe(const char* path)
{
    if (!path || path[0] == '\0') {
        return false;
    }
    for (const char* p = path; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20) {
            return false;
        }
        if (p[0] == '.' && p[1] == '.') {
            char prev = (p == path) ? '/' : *(p - 1);
            char next = *(p + 2);
            bool prev_sep = (prev == '/' || prev == '\\');
            bool next_sep = (next == '/' || next == '\\' || next == '\0');
            if (prev_sep && next_sep) {
                return false;
            }
        }
    }
    return true;
}

static int
spa_read_fd_all(int fd, size_t size, char** out_buf, size_t* out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    if (size == 0) {
        return -1;
    }
    char* buf = malloc(size);
    if (!buf) {
        return -1;
    }
    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    if (total == 0) {
        free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_len = total;
    return 0;
}

static int
spa_read_index_file(const char* doc_root, char** out_buf, size_t* out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    if (!spa_path_is_safe(doc_root)) {
        return -1;
    }
    char path[PATH_MAX];
    int  n = snprintf(path, sizeof(path), "%s/index.html", doc_root);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (size_t)st.st_size > CSILK_SPA_MAX_CACHE_BYTES) {
        close(fd);
        return -1;
    }
    int rc = spa_read_fd_all(fd, (size_t)st.st_size, out_buf, out_len);
    close(fd);
    return rc;
}

static void
spa_work_cb(csilk_io_work_t* req)
{
    spa_work_data_t* data = (spa_work_data_t*)req->data;
    if (!data) {
        return;
    }
    data->status_code = CSILK_STATUS_NOT_FOUND;

    char path[PATH_MAX];
    int  n = snprintf(path, sizeof(path), "%s/index.html", data->doc_root);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return;
    }

    if (data->allow_sendfile) {
        data->fd = fd;
        data->total_size = (size_t)st.st_size;
        data->status_code = CSILK_STATUS_OK;
        return;
    }

    if ((size_t)st.st_size > CSILK_SPA_MAX_CACHE_BYTES) {
        close(fd);
        return;
    }
    if (spa_read_fd_all(fd, (size_t)st.st_size, &data->body, &data->body_len) == 0) {
        data->status_code = CSILK_STATUS_OK;
    }
    close(fd);
}

static void
spa_after_work_cb(csilk_io_work_t* req, int status)
{
    spa_work_data_t* data = (spa_work_data_t*)req->data;
    if (!data) {
        return;
    }
    csilk_async_token_t token = data->token;
    csilk_ctx_t*        c = token.ctx;
    req->data = c;

    bool valid = (status >= 0) && csilk_async_token_validate(&token) &&
                 !atomic_load_explicit(&c->stream_closed, memory_order_acquire);
    if (!valid) {
        if (data->fd >= 0) {
            close(data->fd);
        }
        free(data->body);
        free(data);
        csilk_ctx_release_async(&token);
        return;
    }

    if (data->status_code == CSILK_STATUS_OK) {
        if (data->fd >= 0) {
            csilk_set_header(c, "Content-Type", "text/html");
            csilk_status(c, CSILK_STATUS_OK);
            csilk_set_file_response(c, data->fd, 0, data->total_size);
            data->fd = -1;
        } else if (data->body) {
            csilk_set_header(c, "Content-Type", "text/html");
            csilk_status(c, CSILK_STATUS_OK);
            csilk_set_response_body_ex(c, data->body, data->body_len, CSILK_OWN_HEAP);
            data->body = NULL;
        } else {
            csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        }
    } else {
        if (data->fd >= 0) {
            close(data->fd);
            data->fd = -1;
        }
        free(data->body);
        data->body = NULL;
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    }

    _csilk_send_response(c);
    free(data);
    csilk_ctx_release_async(&token);
}

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
    csilk_server_t* server =
        (client && client->server) ? client->server : (csilk_server_t*)c->server;
    if (!server) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }

    char doc_root_copy[PATH_MAX];
    csilk_mutex_lock(&server->config_mutex);
    if (server->spa_index_buf && server->spa_index_len > 0) {
        size_t            len = server->spa_index_len;
        csilk_ownership_t ownership = c->arena ? CSILK_OWN_ARENA : CSILK_OWN_HEAP;
        char*             body = c->arena ? csilk_arena_alloc(c->arena, len) : malloc(len);
        if (body) {
            memcpy(body, server->spa_index_buf, len);
        }
        csilk_mutex_unlock(&server->config_mutex);
        if (!body) {
            csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
            return;
        }
        csilk_response_body_release(c);
        c->response.body = body;
        c->response.body_len = len;
        c->response.body_capacity = 0;
        c->response.body_ownership = ownership;
        csilk_set_header(c, "Content-Type", "text/html");
        csilk_status(c, CSILK_STATUS_OK);
        return;
    }
    if (!server->spa_doc_root || !spa_path_is_safe(server->spa_doc_root)) {
        csilk_mutex_unlock(&server->config_mutex);
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    int rn = snprintf(doc_root_copy, sizeof(doc_root_copy), "%s", server->spa_doc_root);
    csilk_mutex_unlock(&server->config_mutex);
    if (rn < 0 || (size_t)rn >= sizeof(doc_root_copy)) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }

    csilk_io_loop_t* loop = _csilk_ctx_loop(c);
    if (!loop) {
        csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        return;
    }

    spa_work_data_t* data = calloc(1, sizeof(spa_work_data_t));
    if (!data) {
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
        return;
    }
    data->fd = -1;
    data->status_code = CSILK_STATUS_NOT_FOUND;
    data->allow_sendfile = (client && !client->ssl && client->protocol == CSILK_PROTO_HTTP1);
    snprintf(data->doc_root, sizeof(data->doc_root), "%s", doc_root_copy);

    csilk_ctx_set_async(c, 1);
    data->token = csilk_ctx_acquire_async(c);
    csilk_io_work_t* req = csilk_get_work_req(c);
    req->data = data;

    int rc = csilk_io_queue_work(loop, req, spa_work_cb, spa_after_work_cb);
    if (rc != 0) {
        CSILK_LOG_E("SPA fallback: failed to queue async read: %d", rc);
        csilk_async_token_t tok = data->token;
        req->data = c;
        free(data);
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
        _csilk_send_response(c);
        csilk_ctx_release_async(&tok);
    }
}

/* --- Server configuration --- */

/** @brief Set a custom handler for unmatched routes (404 Not Found).
 *
 * Locks the config mutex so that concurrent updates from
 * csilk_server_set_spa_fallback cannot race on not_found_handler.
 */
void
csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler)
{
    if (!server) {
        return;
    }
    csilk_mutex_lock(&server->config_mutex);
    server->not_found_handler = handler;
    csilk_mutex_unlock(&server->config_mutex);
}

/** @brief Enable SPA fallback: all unmatched GET requests serve index.html.
 *
 * Rejects path traversal / control characters, pre-reads index.html into an
 * in-memory cache when available, and updates spa_doc_root / spa_index_buf /
 * not_found_handler under config_mutex.
 */
void
csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root)
{
    if (!server || !doc_root) {
        return;
    }
    if (!spa_path_is_safe(doc_root)) {
        CSILK_LOG_W("SPA fallback: rejected unsafe doc_root path");
        return;
    }
    char* new_root = strdup(doc_root);
    if (!new_root) {
        return;
    }
    char*  new_buf = NULL;
    size_t new_len = 0;
    (void)spa_read_index_file(doc_root, &new_buf, &new_len);

    csilk_mutex_lock(&server->config_mutex);
    char* old_root = server->spa_doc_root;
    char* old_buf = server->spa_index_buf;
    server->spa_doc_root = new_root;
    server->spa_index_buf = new_buf;
    server->spa_index_len = new_len;
    server->not_found_handler = spa_fallback_handler;
    csilk_mutex_unlock(&server->config_mutex);

    free(old_root);
    free(old_buf);
}

/** @brief Register a global middleware handler that runs before every request.
 *
 * Synchronized under server->config_mutex. Must be called before csilk_server_run().
 */
int
csilk_server_use(csilk_server_t* server, csilk_handler_t handler)
{
    if (!server || !handler) {
        return -1;
    }
    csilk_mutex_lock(&server->config_mutex);
    if (server->middleware_count >= 32) {
        csilk_mutex_unlock(&server->config_mutex);
        CSILK_LOG_E("Server: global middleware limit (32) reached. Middleware "
                    "dropped.");
        return -1;
    }
    server->middlewares[server->middleware_count++] = handler;
    int             count = server->middleware_count;
    csilk_router_t* router = atomic_load_explicit(&server->router, memory_order_acquire);
    if (router) {
        csilk_router_compile(router, server->middlewares, (size_t)count);
    }
    csilk_mutex_unlock(&server->config_mutex);
    CSILK_LOG_D("Server: registered global middleware %p (count: %d)", (void*)handler, count);
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

    /* Run registered subsystem teardowns (MQ, H3, WS rooms, session store, OTLP)
     * in LIFO order before draining worker pools and destroying mutexes. */
    while (server->teardown_cb_count > 0) {
        csilk_server_teardown_fn cb = server->teardown_cbs[--server->teardown_cb_count];
        if (cb) {
            cb(server);
        }
    }

    free(server->spa_doc_root);
    server->spa_doc_root = NULL;
    free(server->spa_index_buf);
    server->spa_index_buf = NULL;
    server->spa_index_len = 0;
    if (server->worker_pools) {
        for (int w = 0; w < server->worker_pool_count; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
            _csilk_worker_drain_dispatch(wp);
            if (wp->dispatch_async.type != 0 &&
                !csilk_io_is_closing((csilk_io_handle_t*)&wp->dispatch_async)) {
                csilk_io_close((csilk_io_handle_t*)&wp->dispatch_async, NULL);
                _server_free_drain_loop(server->loop);
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
        /* NOTE: server->worker_pools itself must outlive csilk_io_loop_close()
         * below. On the io_uring backend, csilk_io_close() only *queues* a
         * deferred close op; the queue is drained inside
         * csilk_io_loop_close(), which then dereferences handles embedded in
         * worker_pool_t (the per-worker dispatch async). Freeing the array
         * here would turn that drain into a use-after-free.
         *
         * The per-worker buffers freed above are owned by the worker_pool_t
         * slots and are unaffected by moving the array free later. */
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
    _csilk_plugin_cleanup_server(server);
    /* _csilk_reload_mgr_free() performs a bounded grace-period wait and skips
     * reclaiming any retired routers/handles still pinned by a stuck reader. */
    _csilk_reload_mgr_free(server);
    _csilk_dispatch_pool_cleanup();

    if (server->loop_owned && server->loop) {
        /* Bounded drain: each close callback flies on the next iteration. */
        for (int i = 0; i < SERVER_FREE_LOOP_DRAIN_MAX && csilk_io_loop_close(server->loop) != 0;
             i++) {
            csilk_io_run(server->loop, CSILK_IO_RUN_NOWAIT);
        }
        free(server->loop);
        server->loop = NULL;
    }

    /* Free after the loop close above: the io_uring backend drains pending
     * close operations inside csilk_io_loop_close(), and those completions
     * still dereference worker_pool_t slots (dispatch async handles). */
    free(server->worker_pools);
    server->worker_pools = NULL;

    csilk_arena_flush_free_list();
    csilk_body_pool_cleanup();
    free(server);
}

/* --- Server stats --- */

/**
 * @brief Authoritative active-connection count.
 *
 * Pool-level counters are the source of truth when worker pools exist
 * (maintained on every acquire / release in pool_get / pool_put).
 * Fall back to the server-level atomic only in single-thread / no-pool mode.
 * Both get_stats() and check_backpressure() call this so they always agree.
 */
static int
_csilk_server_active_total(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    if (server->worker_pools && server->worker_pool_count > 0) {
        int total = 0;
        for (int w = 0; w < server->worker_pool_count; w++) {
            total += atomic_load_explicit(&server->worker_pools[w].active_connections,
                                          memory_order_relaxed);
        }
        return total;
    }
    return atomic_load_explicit(&server->active_connections, memory_order_relaxed);
}

/** @brief Read the server's live connection statistics. */
void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
    if (!server) {
        return;
    }
    int active_total = _csilk_server_active_total(server);
    int pooled_total = 0;

    if (server->worker_pools && server->worker_pool_count > 0) {
        for (int w = 0; w < server->worker_pool_count; w++) {
            pooled_total += atomic_load_explicit(&server->worker_pools[w].client_pool_count,
                                                 memory_order_relaxed);
        }
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

int
csilk_server_get_config(const csilk_server_t* server, csilk_server_config_t* out)
{
    if (!server || !out) {
        return -1;
    }
    csilk_mutex_lock((csilk_mutex_t*)&server->config_mutex);
    *out = server->config;
    csilk_mutex_unlock((csilk_mutex_t*)&server->config_mutex);
    return 0;
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
        size_t total_active = (size_t)_csilk_server_active_total(server);
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

    csilk_mutex_lock(&server->config_mutex);
    csilk_router_t* run_router = atomic_load_explicit(&server->router, memory_order_acquire);
    if (run_router && server->middleware_count > 0) {
        csilk_router_compile(run_router, server->middlewares, (size_t)server->middleware_count);
    }
    char* spa_root_to_warm = NULL;
    if (server->spa_doc_root && !server->spa_index_buf) {
        spa_root_to_warm = strdup(server->spa_doc_root);
    }
    csilk_mutex_unlock(&server->config_mutex);

    if (spa_root_to_warm) {
        char*  warm_buf = NULL;
        size_t warm_len = 0;
        if (spa_read_index_file(spa_root_to_warm, &warm_buf, &warm_len) == 0) {
            csilk_mutex_lock(&server->config_mutex);
            if (!server->spa_index_buf && server->spa_doc_root &&
                strcmp(server->spa_doc_root, spa_root_to_warm) == 0) {
                server->spa_index_buf = warm_buf;
                server->spa_index_len = warm_len;
                warm_buf = NULL;
            }
            csilk_mutex_unlock(&server->config_mutex);
            free(warm_buf);
        }
        free(spa_root_to_warm);
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
    size_t worker_pool_size = (size_t)workers * sizeof(worker_pool_t);
    size_t alloc_size = (worker_pool_size + 63) & ~(size_t)63;
    server->worker_pools = aligned_alloc(64, alloc_size);
    if (server->worker_pools) {
        memset(server->worker_pools, 0, alloc_size);
    }
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

    if (server->h3_state && server->h3_state->port > 0 && server->h3_state->worker_init) {
        server->h3_state->worker_init(server, &server->worker_pools[0], server->loop);
    }

    if (server->config.tcp_keepalive > 0) {
        csilk_io_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
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

    if (workers > 1) {
        CSILK_LOG_I("Server: spawning %d worker threads...", workers - 1);
        if (csilk_spawn_workers(server, workers, port) != 0) {
            csilk_io_close((csilk_io_handle_t*)&server->sig_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->async_handle, NULL);
            csilk_io_close((csilk_io_handle_t*)&server->server_handle, NULL);
            return -1;
        }
    }

    CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

    _csilk_trigger_hooks(server, NULL, CSILK_HOOK_SERVER_START);

    int ret = csilk_io_run(server->loop, CSILK_IO_RUN_DEFAULT);
    /* Drain remaining closing handles (TCP + 4 timers per client) until
     * csilk_io_loop_alive() returns 0 or a safety cap is reached.  Running
     * CSILK_IO_RUN_ONCE lets libuv wait briefly for pending close callbacks
     * that fire after stop_async returns. */
    for (int i = 0; i < 1000 && csilk_io_loop_alive(server->loop); i++) {
        csilk_io_run(server->loop, CSILK_IO_RUN_NOWAIT);
    }
    if (server->worker_pools && server->worker_pool_count > 0) {
        worker_pool_t* wp0 = &server->worker_pools[0];
        _csilk_worker_drain_dispatch(wp0);
        _csilk_worker_cleanup_leftover_clients(server, wp0, server->loop);
        _csilk_worker_drain_dispatch(wp0);
    }
    return ret;
}

/* --- Accessors & RCU Router Management --- */

/** @brief Get the internal message queue instance for the server. */
