/**
 * @file connection.c
 * @brief Connection pool, accept, I/O, timers, and lifecycle callbacks.
 *
 * Implements client connection pooling, accept handling (on_new_connection),
 * TCP read/write callbacks, idle/timeout timers, and client lifecycle
 * (on_close, on_timer_close).  All I/O flows through the on_read callback
 * which dispatches to TLS, WebSocket, or HTTP/1.1 protocol handlers.
 * @copyright MIT License
 */

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "../srv_internal.h"
#include "../ctx_internal.h"
#include "../http/h2.h"
#include "../srv_impl.h"

/* --- Buffer allocation --- */

/** @brief I/O buffer allocation callback — allocates a receive buffer.
 *
 * Allocates a buffer of the suggested size using malloc. The buffer is freed
 * by the I/O backend after the read callback is invoked (libuv path).
 *
 * /* * @param handle          The I/O handle that will read into the buffer.
 * @param suggested_size  Recommended buffer size from the I/O backend.
 * @param buf             [out] Pointer to the csilk_io_buf_t to populate. */
void
alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf)
{
    (void)handle;
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

/* --- Connection pool (per-worker, lock-free) --- */

/** @brief Get a client connection object from the worker-local free pool or
 *  allocate a new one.
 *
 * In multi-worker mode, each worker thread has its own pool with no shared
 * state — pool_get is a pure thread-local O(1) operation with zero locking.
 *
 * @param wp The worker pool (must not be nullptr).
 * @return A csilk_client_t ready for use, or nullptr on allocation failure. */
static csilk_client_t*
pool_get(worker_pool_t* wp)
{
    csilk_client_t* client;
    if (wp->client_pool_count > 0) {
        client = wp->client_pool[--wp->client_pool_count];
    } else {
        client = calloc(1, sizeof(csilk_client_t));
    }
    if (client) {
        client->ctx.file_fd = -1;
    }
    return client;
}

/** @brief Return a client to the worker-local free pool for reuse.
 *
 * SSL and H2 sessions are cleaned before returning. The client struct is
 * zeroed. If the pool has room, the client is saved for reuse; otherwise
 * freed. No lock — only the owning event-loop thread accesses this.
 *
 * @param wp     The worker pool (derived from client->owner_pool).
 * @param client The client to return (must not be used after this call). */
static void
pool_put(worker_pool_t* wp, csilk_client_t* client)
{
    if (client->ssl) {
        SSL_free(client->ssl);
        client->ssl = nullptr;
        client->read_bio = nullptr;
        client->write_bio = nullptr;
    }
    if (client->h2_session) {
        nghttp2_session_del(client->h2_session);
        client->h2_session = nullptr;
    }
    csilk_h2_free_streams(client);
    memset(client, 0, sizeof(*client));
    if (wp->client_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->client_pool[wp->client_pool_count++] = client;
    } else {
        free(client);
    }
}

/** @brief Get a pre-allocated arena from the worker-local arena pool.
 *
 * Pops a pre-allocated arena from the pool. If the pool is empty, falls back
 * to creating a new arena on the fly. Pre-allocated arenas already have their
 * first chunk ready, so the hot path avoids aligned_alloc entirely.
 *
 * @param wp The worker pool (must not be nullptr).
 * @return A csilk_arena_t ready for use, or nullptr on allocation failure. */
static csilk_arena_t*
pool_get_arena(worker_pool_t* wp)
{
    csilk_arena_t* arena;
    if (wp->arena_pool_count > 0) {
        arena = wp->arena_pool[--wp->arena_pool_count];
    } else {
        arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        if (arena && wp->server->config.enable_arena_alignment) {
            csilk_arena_set_alignment(arena, 1);
        }
    }
    return arena;
}

/** @brief Return an arena to the worker-local arena pool for reuse.
 *
 * Resets the arena (zero-clear, no system calls) and pushes it back into
 * the pool. If the pool is full, frees the arena normally.
 *
 * @param wp    The worker pool.
 * @param arena The arena to return (must not be used after this call). */
static void
pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena)
{
    csilk_arena_reset(arena);
    if (wp->arena_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->arena_pool[wp->arena_pool_count++] = arena;
    } else {
        csilk_arena_free(arena);
    }
}

/** @brief Pre-populate the worker-local arena pool with ready-to-use arenas.
 *
 * Each pre-allocated arena has its first chunk already allocated and reset,
 * so the hot accept path (on_new_connection) performs zero aligned_alloc
 * calls — the arena is popped from the pool and used immediately.
 *
 * Alignment is set according to the server config at pre-alloc time so it
 * does not need to be repeated per-connection.
 *
 * @param wp The worker pool to initialise. */
void
_csilk_worker_init_arena_pool(worker_pool_t* wp)
{
    int align = wp->server->config.enable_arena_alignment;
    for (int i = 0; i < CSILK_CLIENT_POOL_SIZE; i++) {
        csilk_arena_t* a = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        if (!a) {
            break;
        }
        if (align) {
            csilk_arena_set_alignment(a, 1);
        }
        /* Pre-allocate the first chunk so csilk_arena_alloc in the hot
         * path always hits the fast (bump) path. */
        void* p = csilk_arena_alloc(a, 1);
        if (!p) {
            csilk_arena_free(a);
            break;
        }
        csilk_arena_reset(a);
        wp->arena_pool[wp->arena_pool_count++] = a;
    }
}

/* --- Active client list --- */

/** @brief Insert a client at the head of the server's active client list.
 *
 * Thread-safe: acquires the clients_mutex before modification.
 *
 * @param server The server instance.
 * @param client The client to add (must not already be in a list). */
static void
client_list_add(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
    worker_pool_t* wp = client->owner_pool;
    client->next = wp->active_clients;
    client->prev = nullptr;
    if (wp->active_clients) {
        wp->active_clients->prev = client;
    }
    wp->active_clients = client;
}

/** @brief Remove a client from the active list (no locking).
 *
 * Unlinks the client from the doubly-linked list and clears its prev/next
 * pointers. Caller must hold clients_mutex.
 *
 * @param server The server instance.
 * @param client The client to remove. */
static void
client_list_remove_internal(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
    worker_pool_t* wp = client->owner_pool;
    if (client->prev) {
        client->prev->next = client->next;
    } else if (wp->active_clients == client) {
        wp->active_clients = client->next;
    }
    if (client->next) {
        client->next->prev = client->prev;
    }
    client->next = client->prev = nullptr;
}

/** @brief Remove a client from the active list (thread-safe).
 *
 * Acquires clients_mutex, then delegates to client_list_remove_internal().
 *
 * @param server The server instance.
 * @param client The client to remove. */
static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
    client_list_remove_internal(server, client);
}

/* --- Timer close --- */

/** @brief Decrement active connections, clean up the request context, return
 *  the arena and client struct to their respective pools.
 *
 *  This is the final teardown step for a client connection. It must only be
 *  called when all references are released (all timers closed AND async_ref
 *  zeroed).
 *
 *  @param client The client connection to destroy (must not be used after). */
static void
client_destroy(csilk_client_t* client)
{
    if (client->server) {
        atomic_fetch_sub(&client->server->active_connections, 1);
    }
    csilk_ctx_cleanup(&client->ctx);
    if (client->ctx.arena) {
        pool_put_arena(client->owner_pool, client->ctx.arena);
    }
    pool_put(client->owner_pool, client);
}

/** @brief Get the I/O event loop associated with the request context.
 *
 *  Extracts the loop from the internal client's TCP handle.  Falls back to
 *  csilk_io_default_loop() if the context chain is incomplete (e.g. during early
 *  initialization or in unit tests).
 *
 *  @param c The request context.
 *  @return A pointer to the owning I/O event loop (never nullptr). */
CSILK_INTERNAL csilk_io_loop_t*
_csilk_ctx_loop(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return csilk_io_default_loop();
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    return client->handle.loop;
}

/** @brief Increment the async reference counter for the client connection.
 *
 *  Prevents premature client destruction while an async operation (e.g.
 *  thread-pool work, streaming write) is in flight.  Each incr must be
 *  paired with a matching decr.
 *
 *  @param c The request context. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_incr(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    client->async_ref++;
}

/** @brief Decrement the async reference counter; destroy client if last ref.
 *
 *  When async_ref reaches 0 AND close_pending is 0 AND the connection is
 *  already marked closed (conn_closed), the client is fully destroyed.
 *  This prevents both leaks (abandoned clients) and use-after-free (relying
 *  on just one condition).
 *
 *  @param c The request context. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_decr(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    client->async_ref--;
    if (client->async_ref <= 0 && client->close_pending <= 0 && c->conn_closed) {
        client_destroy(client);
    }
}

/** @brief Close callback for timer handles — decrements close_pending
 *  and triggers client_destroy when all timers are closed.
 *
 *  Each of the four timers (idle, read, write, request) calls this once on
 *  close.  Client destruction is deferred until all four have acknowledged
 *  AND async_ref is zero.
 *
 *  @param handle The timer handle being closed (data points to csilk_client_t). */
static void
on_timer_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!client) {
        return;
    }
    client->close_pending--;
    if (client->close_pending > 0) {
        return;
    }
    if (client->async_ref > 0) {
        return;
    }
    client_destroy(client);
}

/* --- Connection close --- */

/** @brief Close callback for client TCP handles — performs full cleanup.
 *
 * Triggers the CSILK_HOOK_CONN_CLOSE hook, removes the client from the
 * active connections list, stops all four timers, and initiates their close
 * via on_timer_close. When all timers are closed, the client's request
 * context, arena, and temporary buffers are freed and the client is returned
 * to the pool.
 *
 * @param handle The TCP handle being closed (data points to csilk_client_t).
 */
CSILK_INTERNAL void
on_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client) {
        CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
        _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
        client_list_remove(client->server, client);
        client->ctx.conn_closed = 1;
        csilk_io_timer_stop(&client->timer);
        csilk_io_timer_stop(&client->read_timer);
        csilk_io_timer_stop(&client->write_timer);
        csilk_io_timer_stop(&client->request_timer);

        client->close_pending = 4;
        csilk_io_handle_t* timers[] = {(csilk_io_handle_t*)&client->timer,
                                       (csilk_io_handle_t*)&client->read_timer,
                                       (csilk_io_handle_t*)&client->write_timer,
                                       (csilk_io_handle_t*)&client->request_timer};
        for (int i = 0; i < 4; i++) {
            if (csilk_io_is_closing(timers[i])) {
                client->close_pending--;
            } else {
                timers[i]->data = client;
                csilk_io_close(timers[i], on_timer_close);
            }
        }
        if (client->close_pending <= 0) {
            if (client->async_ref > 0) {
                return;
            }
            client_destroy(client);
        }
    }
}

/* --- Timer callbacks --- */

/** @brief Timer callback: fired when no I/O activity occurs within the
 *  idle timer window (keep-alive timeout).
 *
 *  Closes the connection gracefully, which triggers the on_close chain.
 *  Skips close if already closing to avoid double-close.
 *
 *  @param handle The idle timer handle (data points to csilk_client_t). */
void
on_idle_timeout(uv_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to idle timeout");
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: fired when no request data has been received
 * within read_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data). */
void
on_read_timeout(uv_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to read timeout");
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: fired when the response write has not
 * completed within write_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data). */
void
on_write_timeout(uv_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/* --- Rejected connection --- */

/** @brief Close callback for rejected (connection-limited) TCP handles.
 *
 *  When the server reaches max_connections, excess connections are accepted
 *  and immediately closed. The handle (a temporary uv_tcp_t allocated in
 *  on_new_connection) is freed here. This drains the kernel TCP backlog
 *  without allocating a full csilk_client_t.
 *
 *  @param handle The temporary TCP handle to free. */
static void
on_rejected_close(csilk_io_handle_t* handle)
{
    free(handle);
}

/* --- Accept new connection --- */

/** @brief Connection callback — accept a new incoming TCP connection.
 *
 * This is the entry point for every new TCP connection. The sequence is:
 *
 *   1. Connection limiter: if active_connections >= max_connections, accept
 *      and immediately close (drains the kernel backlog without processing).
 *
 *   2. Client acquisition: get a client struct from the pool (pool_get).
 *      Pool reuse avoids calloc/free churn for every connection.
 *
 *   3. TCP handle init: uv_tcp_init + uv_accept to attach the fd.
 *      TCP_NODELAY is applied if configured (disables Nagle's algorithm).
 *
 *   4. Counters: atomic_fetch_add active_connections.
 *
 *   5. Parser init: llhttp_init with the server's callback table.
 *      The parser state machine is reset for each new connection.
 *
 *   6. TLS setup: if ssl_ctx is configured, set up BIO pairs and start
 *      the TLS handshake (setup_client_tls).
 *
 *   7. Timer setup: read_timeout and request_timeout are one-shot timers
 *      that fire if no data arrives within the configured window.
 *
 *   8. Arena init: per-connection bump allocator for request-scoped
 *      allocations (path strings, query params, arena handler chains).
 *
 *   9. Read: csilk_io_read_start registers the on_read callback with
 *      the I/O backend (libuv or io_uring).
 *
 * If any step fails (allocation, accept, init), the client is cleaned up
 * via close callbacks and returned to the pool.
 *
 * @param server_stream The listening server stream.
 * @param status        Connection status (negative on error). */
void
on_new_connection(csilk_io_stream_t* server_stream, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Connection: new connection error: %s", csilk_io_strerror(status));
        return;
    }

    worker_pool_t*  wp = (worker_pool_t*)server_stream->data;
    csilk_server_t* server = wp->server;

    int max_conn = server->config.max_connections;
    if (max_conn == 0) {
        max_conn = server->max_connections;
    }
    if (max_conn > 0 && atomic_load(&server->active_connections) >= max_conn) {
        printf("REJECTING CONNECTION max=%d active=%d\n",
               max_conn,
               atomic_load(&server->active_connections));
        fflush(stdout);
        uv_tcp_t* tmp = malloc(sizeof(uv_tcp_t));
        if (tmp) {
            uv_tcp_init(server_stream->loop, tmp);
            if (uv_accept(server_stream, (csilk_io_stream_t*)tmp) == 0) {
                csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
            } else {
                csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
            }
        }
        return;
    }

    csilk_client_t* client = pool_get(wp);
    if (!client) {
        uv_tcp_t* tmp = malloc(sizeof(uv_tcp_t));
        if (tmp) {
            uv_tcp_init(server_stream->loop, tmp);
            if (uv_accept(server_stream, (csilk_io_stream_t*)tmp) == 0) {
                csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
            } else {
                csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
            }
        }
        return;
    }

    client->server = server;
    client->owner_pool = wp;
    int r = uv_tcp_init(server_stream->loop, &client->handle);
    if (r < 0) {
        CSILK_LOG_E("Connection: uv_tcp_init error: %s", csilk_io_strerror(r));
        pool_put(wp, client);
        return;
    }
    client->handle.data = client;

    _csilk_ctx_init(&client->ctx, server, client);
    client->ctx.arena = pool_get_arena(wp);

    client_list_add(server, client);

    if (uv_accept(server_stream, (csilk_io_stream_t*)&client->handle) == 0) {
        CSILK_LOG_D("Connection: accepted new TCP connection (client pointer: %p)", (void*)client);
        if (server->config.tcp_nodelay) {
            uv_tcp_nodelay((uv_tcp_t*)&client->handle, 1);
        }
        atomic_fetch_add(&server->active_connections, 1);
        client->protocol = CSILK_PROTO_HTTP1;
        llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
        client->parser.data = client;

        _csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

        if (server->ssl_ctx) {
            CSILK_LOG_D("Connection: setting up TLS for connection: %p", (void*)client);
            if (setup_client_tls(client) < 0) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
                return;
            }
        }

        uv_timer_init(server_stream->loop, &client->timer);
        client->timer.data = client;
        uv_timer_init(server_stream->loop, &client->read_timer);
        client->read_timer.data = client;
        uv_timer_init(server_stream->loop, &client->write_timer);
        client->write_timer.data = client;
        uv_timer_init(server_stream->loop, &client->request_timer);
        client->request_timer.data = client;

        CSILK_LOG_T("Connection: connection timers initialized, starting read listener");
        if (server->config.read_timeout_ms > 0) {
            csilk_io_timer_start(
                &client->read_timer, on_read_timeout, server->config.read_timeout_ms, 0);
        }
        if (server->config.request_timeout_ms > 0) {
            csilk_io_timer_start(
                &client->request_timer, on_read_timeout, server->config.request_timeout_ms, 0);
        }

        r = uv_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
        if (r < 0) {
            CSILK_LOG_E("Connection: uv_read_start error: %s", csilk_io_strerror(r));
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
    } else {
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }
}

/* --- TCP read --- */

/** @brief I/O read callback — processes incoming data from a client
 * connection.
 *
 * This is the heart of the event-driven I/O model. Every byte from every
 * connection arrives here. The dispatch logic has three paths:
 *
 *   TLS path (client->ssl is set):
 *     Data is written to the read BIO, then process_tls_read() drives the
 *     TLS handshake (if not yet complete) or decrypts and feeds the result
 *     to the llhttp parser (or WebSocket frame parser). Encrypted output
 *     from the write BIO is flushed via flush_tls_write().
 *
 *   WebSocket path (client->ctx.is_websocket):
 *     Data is parsed directly as WebSocket frames by csilk_ws_parse_frame().
 *     No HTTP parsing occurs on this connection after the upgrade.
 *
 *   HTTP path (default):
 *     Data is fed directly to llhttp_execute(). The callbacks in
 *     server->settings (on_url, on_header_field, on_body, etc.) incrementally
 *     build the request struct. When the request is complete,
 *     on_message_complete fires to dispatch routing.
 *
 * On positive nread: feed data to the appropriate handler.
 * On nread == UV_EOF: peer closed the connection; close the client.
 * On nread < 0 (error): log and close.
 *
 * The idle timer is always stopped when data arrives (keep-alive wait
 * is reset). The read timeout is restarted.
 *
 * @param stream The client TCP stream.
 * @param nread  Number of bytes read (negative for error/EOF).
 * @param buf    The buffer that was read into (freed by this callback). */
void
on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf)
{
    csilk_client_t* client = (csilk_client_t*)stream->data;
    char*           base = buf->base;
    int             is_registered = 0;
    csilk_io_timer_stop(&client->timer);
    if (client->server->config.read_timeout_ms > 0) {
        csilk_io_timer_start(
            &client->read_timer, on_read_timeout, client->server->config.read_timeout_ms, 0);
    }
    if (nread > 0) {
        if (client->ssl) {
            BIO_write(client->read_bio, base, (int)nread);
            process_tls_read(client);
        } else if (client->ctx.is_websocket) {
            csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
        } else {
            /* Register the receive buffer so it stays alive for zero-copy header/body views. */
            if (client->ctx.read_buffers_count < 16) {
                client->ctx.read_buffers[client->ctx.read_buffers_count++] = base;
                is_registered = 1;
            } else {
                CSILK_LOG_W("Connection: read_buffers capacity exceeded, freeing "
                            "immediately");
                free(base);
                base = nullptr;
            }

            if (base) {
                enum llhttp_errno err = llhttp_execute(&client->parser, base, nread);
                if (err == HPE_CLOSED_CONNECTION) {
                    llhttp_init(&client->parser, HTTP_REQUEST, &client->server->settings);
                    client->parser.data = client;
                } else if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
                    CSILK_LOG_E("Connection: HTTP parse error: %s %s",
                                llhttp_errno_name(err),
                                client->parser.reason ? client->parser.reason : "unknown reason");

                    if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                        csilk_io_close((csilk_io_handle_t*)stream, on_close);
                    }
                }
            }
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            CSILK_LOG_E("Connection: read error: %s", uv_err_name((int)nread));
        }
        if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
            csilk_io_close((csilk_io_handle_t*)stream, on_close);
        }
    }

    if (base && !is_registered) {
        free(base);
    }
}

/* --- Get client IP --- */

/** @brief Get the remote client's IP address as a string.
 *
 * Resolves the client's IP address (IPv4 or IPv6) from the underlying TCP
 * socket using the I/O backend's getpeername. The result is allocated in arena memory
 * so it is valid for the duration of the request.
 *
 * @param c The request context.
 * @return A string with the client IP (e.g., "127.0.0.1" or "::1"), or nullptr
 *         if the context is nullptr or the address cannot be resolved. */
const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return nullptr;
    }
    csilk_client_t*         client = (csilk_client_t*)c->_internal_client;
    struct sockaddr_storage addr;
    int                     len = sizeof(addr);
    if (uv_tcp_getpeername(&client->handle, (struct sockaddr*)&addr, &len) == 0) {
        char ip[46];
        if (addr.ss_family == AF_INET) {
            uv_ip4_name((struct sockaddr_in*)&addr, ip, sizeof(ip));
        } else {
            uv_ip6_name((struct sockaddr_in6*)&addr, ip, sizeof(ip));
        }
        return csilk_arena_strdup(c->arena, ip);
    }

    return nullptr;
}

void
csilk_client_read_start(csilk_client_t* client)
{
    uv_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
}

void
csilk_client_read_stop(csilk_client_t* client)
{
    uv_read_stop((csilk_io_stream_t*)&client->handle);
}
