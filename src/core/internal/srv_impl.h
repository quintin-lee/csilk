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
    int max_conn = atomic_load(&server->max_connections);
    if (max_conn <= 0) {
        max_conn = server->config.max_connections;
    }

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

CSILK_INTERNAL void
alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf);

CSILK_INTERNAL void on_close(csilk_io_handle_t* handle);
#ifndef CSILK_USE_URING
CSILK_INTERNAL void on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf);
CSILK_INTERNAL void on_new_connection(csilk_io_stream_t* server_stream, int status);
#else
CSILK_INTERNAL void on_read(csilk_client_t* client, ssize_t nread);
CSILK_INTERNAL void on_new_connection(worker_pool_t* wp, int client_fd);
#endif
CSILK_INTERNAL void _csilk_worker_init_arena_pool(worker_pool_t* wp);
CSILK_INTERNAL void csilk_arena_flush_free_list(void);
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
CSILK_INTERNAL int         on_header_value(llhttp_t* p, const char* at, size_t length);
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
/* These declarations use libuv types and are only needed by the libuv backend. */
typedef struct {
    worker_pool_t*   wp;
    int              port;
    csilk_barrier_t* barrier;
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
