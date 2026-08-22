/**
 * @file srv_impl.h
 * @brief Internal declarations for server implementation files.
 *
 * This header declares functions that are shared across the split server
 * implementation files (server.c, connection.c, http1.c, tls.c).  These
 * functions were previously file-static in the monolithic server.c but
 * must be visible across translation units after the split.
 * @copyright MIT License
 */

#ifndef SRV_IMPL_H
#define SRV_IMPL_H

#include <llhttp.h>
#include <csilk/core/sys_io.h>
#include "csilk/csilk.h"
#include "core/internal/srv_internal.h"

/* --- Connection pool & I/O (connection.c) --- */

/**
 * @brief Atomically try to acquire a connection slot under max_connections.
 *
 * Uses CAS (Compare-And-Swap) to eliminate TOCTOU race conditions across worker
 * threads when checking and reserving connection capacity.
 *
 * @param server The server instance.
 * @return 0 on success (slot reserved), -1 if max_connections reached.
 */
static inline int
_csilk_server_try_acquire_connection(csilk_server_t* server)
{
    if (!server) {
        return -1;
    }
    int max_conn = atomic_load_explicit(&server->max_connections, memory_order_relaxed);
    if (max_conn <= 0) {
        atomic_fetch_add(&server->active_connections, 1);
        return 0;
    }

    int curr = atomic_load(&server->active_connections);
    while (1) {
        if (curr >= max_conn) {
            return -1;
        }
        if (atomic_compare_exchange_weak(&server->active_connections, &curr, curr + 1)) {
            return 0;
        }
    }
}

/**
 * @brief Release a previously reserved connection slot (e.g. on handshake / init failure).
 *
 * @param server The server instance.
 */
static inline void
_csilk_server_release_connection(csilk_server_t* server)
{
    if (server) {
        atomic_fetch_sub(&server->active_connections, 1);
    }
}

/* --- Runtime Config Fast Inline Accessors --- */

static inline size_t
_csilk_server_get_max_body_size(const csilk_server_t* server)
{
    if (!server) {
        return CSILK_DEFAULT_MAX_BODY_SIZE;
    }
    size_t sz = atomic_load_explicit(&server->runtime_config.max_body_size, memory_order_relaxed);
    return sz > 0 ? sz : CSILK_DEFAULT_MAX_BODY_SIZE;
}

static inline size_t
_csilk_server_get_max_header_size(const csilk_server_t* server)
{
    if (!server) {
        return CSILK_DEFAULT_MAX_HEADER_SIZE;
    }
    size_t sz = atomic_load_explicit(&server->runtime_config.max_header_size, memory_order_relaxed);
    return sz > 0 ? sz : CSILK_DEFAULT_MAX_HEADER_SIZE;
}

static inline size_t
_csilk_server_get_max_url_size(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.max_url_size, memory_order_relaxed);
}

static inline size_t
_csilk_server_get_max_headers_count(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.max_headers_count, memory_order_relaxed);
}

static inline unsigned int
_csilk_server_get_idle_timeout_ms(const csilk_server_t* server)
{
    if (!server) {
        return CSILK_DEFAULT_IDLE_TIMEOUT;
    }
    unsigned int t =
        atomic_load_explicit(&server->runtime_config.idle_timeout_ms, memory_order_relaxed);
    return t > 0 ? t : CSILK_DEFAULT_IDLE_TIMEOUT;
}

static inline unsigned int
_csilk_server_get_read_timeout_ms(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.read_timeout_ms, memory_order_relaxed);
}

static inline unsigned int
_csilk_server_get_write_timeout_ms(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.write_timeout_ms, memory_order_relaxed);
}

static inline unsigned int
_csilk_server_get_request_timeout_ms(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.request_timeout_ms, memory_order_relaxed);
}

static inline int
_csilk_server_get_enable_simd(const csilk_server_t* server)
{
    if (!server) {
        return 1;
    }
    return atomic_load_explicit(&server->runtime_config.enable_simd, memory_order_relaxed);
}

static inline int
_csilk_server_get_h2_push_enable(const csilk_server_t* server)
{
    if (!server) {
        return 0;
    }
    return atomic_load_explicit(&server->runtime_config.h2_push_enable, memory_order_relaxed);
}

static inline int
_csilk_server_get_h2_max_push(const csilk_server_t* server)
{
    if (!server) {
        return 10;
    }
    int m =
        atomic_load_explicit(&server->runtime_config.h2_max_push_per_request, memory_order_relaxed);
    return m > 0 ? m : 10;
}

CSILK_INTERNAL void
alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf);

CSILK_INTERNAL csilk_client_t* pool_get(worker_pool_t* wp);
CSILK_INTERNAL void            pool_put(worker_pool_t* wp, csilk_client_t* client);
CSILK_INTERNAL csilk_arena_t*  pool_get_arena(worker_pool_t* wp);
CSILK_INTERNAL void            pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena);
CSILK_INTERNAL void            client_list_add(csilk_server_t* server, csilk_client_t* client);
CSILK_INTERNAL void            client_list_remove(csilk_server_t* server, csilk_client_t* client);
CSILK_INTERNAL void            client_destroy(csilk_client_t* client);
CSILK_INTERNAL int             csilk_client_ref(csilk_client_t* client);
CSILK_INTERNAL int             csilk_client_unref(csilk_client_t* client);
CSILK_INTERNAL int             _csilk_client_pending_io_inc(csilk_client_t* client);
CSILK_INTERNAL int             _csilk_client_pending_io_dec(csilk_client_t* client);
CSILK_INTERNAL void            _csilk_client_check_recycle(csilk_client_t* client);

CSILK_INTERNAL void on_close(csilk_io_handle_t* handle);
CSILK_INTERNAL void on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf);
CSILK_INTERNAL void on_new_connection(csilk_io_stream_t* server_stream, int status);

CSILK_INTERNAL const char* csilk_conn_state_str(csilk_conn_state_t state);
CSILK_INTERNAL bool csilk_conn_is_valid_transition(csilk_conn_state_t from, csilk_conn_state_t to);
CSILK_INTERNAL void csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state);
CSILK_INTERNAL csilk_conn_state_t csilk_conn_get_state(const csilk_client_t* client);

/* --- Centralized Atomic Initializers --- */
CSILK_INTERNAL void _csilk_runtime_config_init(csilk_runtime_config_t*      rc,
                                               const csilk_server_config_t* cfg);
CSILK_INTERNAL void _csilk_server_atomics_init(csilk_server_t* s, csilk_router_t* router);
CSILK_INTERNAL void
_csilk_worker_pool_atomics_init(worker_pool_t* wp, csilk_server_t* server, int worker_index);
CSILK_INTERNAL void _csilk_client_atomics_init(csilk_client_t* client);

CSILK_INTERNAL void                   _csilk_worker_set_current_pool(worker_pool_t* wp);
CSILK_INTERNAL worker_pool_t*         _csilk_worker_get_current_pool(void);
CSILK_INTERNAL csilk_dispatch_task_t* _csilk_dispatch_task_alloc(void);
CSILK_INTERNAL void                   _csilk_dispatch_task_free(csilk_dispatch_task_t* task);

CSILK_INTERNAL void _csilk_worker_init_arena_pool(worker_pool_t* wp);
CSILK_INTERNAL void _csilk_worker_init_read_buf_pool(worker_pool_t* wp);
CSILK_INTERNAL void _csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop);
CSILK_INTERNAL void _csilk_worker_drain_dispatch(worker_pool_t* wp);
CSILK_INTERNAL void _csilk_dispatch_pool_cleanup(void);

CSILK_INTERNAL void csilk_arena_flush_free_list(void);

CSILK_INTERNAL void on_timer_close(csilk_io_handle_t* handle);
CSILK_INTERNAL void on_idle_timeout(csilk_io_timer_t* handle);
CSILK_INTERNAL void on_read_timeout(csilk_io_timer_t* handle);
CSILK_INTERNAL void on_write_timeout(csilk_io_timer_t* handle);

/* --- HTTP/1.1 callbacks & response (http1.c) --- */

CSILK_INTERNAL const char* get_status_text(int status);
CSILK_INTERNAL void        on_write(csilk_io_write_t* req, int status);
CSILK_INTERNAL void        _csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len);
CSILK_INTERNAL int         on_message_begin(llhttp_t* p);
CSILK_INTERNAL int         on_url(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_header_field(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_header_field_complete(llhttp_t* p);
CSILK_INTERNAL int         on_header_value(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_header_value_complete(llhttp_t* p);
CSILK_INTERNAL int         on_headers_complete(llhttp_t* p);
CSILK_INTERNAL int         on_body(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_message_complete(llhttp_t* p);
CSILK_INTERNAL void        _csilk_handle_post_response(csilk_client_t* client, int keep_alive);
CSILK_INTERNAL size_t      _csilk_client_get_write_queue_size(csilk_client_t* client);
CSILK_INTERNAL void        _csilk_check_and_trigger_drain(csilk_client_t* client);

/* --- TLS (tls.c) --- */

CSILK_INTERNAL void init_tls(csilk_server_t* s);
CSILK_INTERNAL void cleanup_tls(csilk_server_t* s);
CSILK_INTERNAL int  setup_client_tls(csilk_client_t* client);
CSILK_INTERNAL void process_tls_read(csilk_client_t* client);
CSILK_INTERNAL void flush_tls_write(csilk_client_t* client);

/* --- Hooks (hooks.c) --- */

CSILK_INTERNAL void csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler);
CSILK_INTERNAL void _csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type);

CSILK_INTERNAL void csilk_client_read_start(csilk_client_t* client);
CSILK_INTERNAL void csilk_client_read_stop(csilk_client_t* client);

/* --- Server split (server_lifecycle.c, server_shutdown.c, server_worker.c) --- */
typedef struct {
    worker_pool_t*   wp;
    int              port;
    csilk_barrier_t* barrier;
    _Atomic int      success;
} worker_data_t;

typedef struct {
    csilk_io_loop_t* loop;
    csilk_io_tcp_t*  listen_handle;
    csilk_server_t*  server;
    int              worker_index;
} worker_stop_data_t;

/* server_shutdown.c */
CSILK_INTERNAL void on_signal(csilk_io_signal_t* handle, int signum);
CSILK_INTERNAL void on_stop_async(csilk_io_async_t* handle);
CSILK_INTERNAL void on_worker_stop_async(csilk_io_async_t* handle);

/* server_worker.c */
CSILK_INTERNAL int  bind_and_listen(csilk_io_loop_t* loop,
                                    csilk_io_tcp_t*  out_handle,
                                    int              port,
                                    int              backlog,
                                    bool             reuseport,
                                    int              worker_index);
CSILK_INTERNAL void worker_thread(void* arg);
CSILK_INTERNAL void _csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop);

#endif /* SRV_IMPL_H */
