/**
 * @file connection_io.c
 * @brief Connection I/O callbacks: accept, read, reject.
 */

#include <openssl/ssl.h>
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* Forward declarations for cross-file references */
static void pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf);
void        alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf);
void        on_read_timeout(csilk_io_timer_t* handle);
void        on_close(csilk_io_handle_t* handle);
void        csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state);
void        client_list_remove(csilk_server_t* server, csilk_client_t* client);
void        on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf);

/* --- Rejected connection --- */

#ifndef CSILK_USE_URING
/** @brief Close callback for rejected (connection-limited) TCP handles.
 *
 *  When the server reaches max_connections, excess connections are accepted
 *  and immediately closed. The handle (a temporary csilk_io_tcp_t allocated in
 *  on_new_connection) is freed here. This drains the kernel TCP backlog
 *  without allocating a full csilk_client_t.
 *
 *  @param handle The temporary TCP handle to free.
 */
static void
on_rejected_close(csilk_io_handle_t* handle)
{
    free(handle);
}
#endif

static void
reject_connection(csilk_io_stream_t* server_stream)
{
#ifdef CSILK_USE_URING
    csilk_io_tcp_t tmp;
    csilk_io_tcp_init(server_stream->loop, &tmp);
    if (csilk_io_accept(server_stream, (csilk_io_stream_t*)&tmp) == 0) {
        csilk_io_close((csilk_io_handle_t*)&tmp, NULL);
    }
#else
    csilk_io_tcp_t* tmp = malloc(sizeof(csilk_io_tcp_t));
    if (tmp) {
        csilk_io_tcp_init(server_stream->loop, tmp);
        if (csilk_io_accept(server_stream, (csilk_io_stream_t*)tmp) == 0) {
            extern void on_rejected_close(csilk_io_handle_t * handle);
            csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
        } else {
            extern void on_rejected_close(csilk_io_handle_t * handle);
            csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
        }
    }
#endif
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
 *   3. TCP handle init: csilk_io_tcp_init + csilk_io_accept to attach the fd.
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
 * @param status        Connection status (negative on error).
 */
void
on_new_connection(csilk_io_stream_t* server_stream, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Connection: new connection error: %s", csilk_io_strerror(status));
        return;
    }

    worker_pool_t*  wp = (worker_pool_t*)server_stream->data;
    csilk_server_t* server = wp->server;

    if (_csilk_server_try_acquire_connection(server) < 0) {
        reject_connection(server_stream);
        return;
    }

    extern csilk_client_t* pool_get(worker_pool_t * wp);
    csilk_client_t*        client = pool_get(wp);
    if (!client) {
        _csilk_server_release_connection(server);
        reject_connection(server_stream);
        return;
    }

    client->server = server;
    client->owner_pool = wp;
    int r = csilk_io_tcp_init(server_stream->loop, &client->handle);
    if (r < 0) {
        CSILK_LOG_E("Connection: csilk_io_tcp_init error: %s", csilk_io_strerror(r));
        _csilk_server_release_connection(server);
        extern void pool_put(worker_pool_t * wp, csilk_client_t * client);
        pool_put(wp, client);
        return;
    }
    client->handle.data = client;

    _csilk_ctx_init(&client->ctx, server, client);
    extern csilk_arena_t* pool_get_arena(worker_pool_t * wp);
    client->ctx.arena = pool_get_arena(wp);

    extern void client_list_add(csilk_server_t * server, csilk_client_t * client);
    client_list_add(server, client);

    if (csilk_io_accept(server_stream, (csilk_io_stream_t*)&client->handle) == 0) {
        CSILK_LOG_D("Connection: accepted new TCP connection (client pointer: %p)", (void*)client);
        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
        csilk_client_ref(client);
        if (server->config.tcp_nodelay) {
            csilk_io_tcp_nodelay(&client->handle, 1);
        }
        client->protocol = CSILK_PROTO_HTTP1;
        llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
        client->parser.data = client;

        _csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

        if (server->ssl_ctx) {
            CSILK_LOG_D("Connection: setting up TLS for connection: %p", (void*)client);
            csilk_conn_set_state(client, CSILK_CONN_TLS);
            extern int setup_client_tls(csilk_client_t * client);
            if (setup_client_tls(client) < 0) {
                extern void on_close(csilk_io_handle_t * handle);
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
                return;
            }
        }

        /* Timer initialization */
        csilk_io_timer_init(server_stream->loop, &client->timer);
        client->timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->read_timer);
        client->read_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->write_timer);
        client->write_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->request_timer);
        client->request_timer.data = client;

        CSILK_LOG_T("Connection: connection timers initialized, starting read listener");
        unsigned int read_timeout = _csilk_server_get_read_timeout_ms(server);
        if (read_timeout > 0) {
            csilk_io_timer_start(&client->read_timer, on_read_timeout, read_timeout, 0);
        }
        unsigned int req_timeout = _csilk_server_get_request_timeout_ms(server);
        if (req_timeout > 0) {
            csilk_io_timer_start(&client->request_timer, on_read_timeout, req_timeout, 0);
        }

        if (!server->ssl_ctx) {
            csilk_conn_set_state(client, CSILK_CONN_READING);
        }

        r = csilk_io_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
        if (r < 0) {
            CSILK_LOG_E("Connection: csilk_io_read_start error: %s", csilk_io_strerror(r));
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                extern void on_close(csilk_io_handle_t * handle);
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
    } else {
        client_list_remove(server, client);
        _csilk_server_release_connection(server);
        if (client->ctx.arena) {
            extern void pool_put_arena(worker_pool_t * wp, csilk_arena_t * arena);
            pool_put_arena(wp, client->ctx.arena);
            client->ctx.arena = NULL;
        }
        extern void pool_put(worker_pool_t * wp, csilk_client_t * client);
        pool_put(wp, client);
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
 * @param buf    The buffer that was read into (freed by this callback).
 */
void
on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf)
{
    csilk_client_t* client = (csilk_client_t*)stream->data;
    char*           base = buf->base;
    int             is_registered = 0;

    if (!client || client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        if (base) {
            extern void pool_put_read_buf(worker_pool_t * wp, char* base, size_t size);
            pool_put_read_buf(NULL, base, buf->len);
        }
        return;
    }

    csilk_io_timer_stop(&client->timer);
    unsigned int read_timeout = _csilk_server_get_read_timeout_ms(client->server);
    if (read_timeout > 0) {
        csilk_io_timer_start(&client->read_timer, on_read_timeout, read_timeout, 0);
    }
    if (nread > 0) {
        if (client->ssl) {
            BIO_write(client->read_bio, base, (int)nread);
            extern void process_tls_read(csilk_client_t * client);
            process_tls_read(client);
        } else if (client->ctx.is_websocket) {
            csilk_conn_set_state(client, CSILK_CONN_STREAMING);
            csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
        } else {
            csilk_conn_set_state(client, CSILK_CONN_READING);

            /* Register the receive buffer so it stays alive for zero-copy header/body views.
             * Use pooled registration when the buffer comes from the worker-local pool
             * (buf->len > 0 indicates a tier-backed buffer). */
            if (_csilk_ctx_register_pooled_read_buffer(&client->ctx, base, buf->len) != 0) {
                CSILK_LOG_E("Connection: failed to register read buffer, out of memory");
                if (base) {
                    extern void pool_put_read_buf(worker_pool_t * wp, char* base, size_t size);
                    pool_put_read_buf(client->owner_pool, base, buf->len);
                }
                if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                    extern void on_close(csilk_io_handle_t * handle);
                    csilk_io_close((csilk_io_handle_t*)stream, on_close);
                }
                return;
            }
            is_registered = 1;

            enum llhttp_errno err = llhttp_execute(&client->parser, base, nread);
            if (err == HPE_CLOSED_CONNECTION) {
                llhttp_init(&client->parser, HTTP_REQUEST, &client->server->settings);
                client->parser.data = client;
            } else if (err != HPE_OK && err != HPE_PAUSED && err != HPE_PAUSED_UPGRADE) {
                CSILK_LOG_E("Connection: HTTP parse error: %s %s",
                            llhttp_errno_name(err),
                            client->parser.reason ? client->parser.reason : "unknown reason");

                if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                    extern void on_close(csilk_io_handle_t * handle);
                    csilk_io_close((csilk_io_handle_t*)stream, on_close);
                }
            }
        }

    } else if (nread < 0) {
        if (nread != -1 && nread != -4095 /* UV_EOF */) {
            CSILK_LOG_E("Connection: read error: %s", csilk_io_err_name((int)nread));
        }
        if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
            extern void on_close(csilk_io_handle_t * handle);
            csilk_io_close((csilk_io_handle_t*)stream, on_close);
        }
    }

    if (base && !is_registered) {
        extern void pool_put_read_buf(worker_pool_t * wp, char* base, size_t size);
        pool_put_read_buf(client->owner_pool, base, buf->len);
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
 * @return A string with the client IP (e.g., "127.0.0.1" or "::1"), or NULL
 *         if the context is NULL or the address cannot be resolved.
 */
const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return NULL;
    }
    csilk_client_t*         client = (csilk_client_t*)c->_internal_client;
    struct sockaddr_storage addr;
    int                     len = sizeof(addr);
    if (csilk_io_tcp_getpeername(&client->handle, (struct sockaddr*)&addr, &len) == 0) {
        char ip[46];
        if (addr.ss_family == AF_INET) {
            csilk_io_ip4_name((const struct sockaddr_in*)&addr, ip, sizeof(ip));
        } else {
            csilk_io_ip6_name((const struct sockaddr_in6*)&addr, ip, sizeof(ip));
        }
        return csilk_arena_strdup(c->arena, ip);
    }

    return NULL;
}

/**
 * @brief Begin reading from a client connection via the loop.
 * @param[in] client Client whose underlying stream is subscribed to reads.
 * @note Installs alloc_buffer/on_read and starts csilk_io_read_start on the client's
 *       stream handle.
 */
void
csilk_client_read_start(csilk_client_t* client)
{
    csilk_io_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
}

/**
 * @brief Stop reading from a client connection.
 * @param[in] client Client whose underlying stream read is halted.
 * @note Calls csilk_io_read_stop on the client's stream handle.
 */
void
csilk_client_read_stop(csilk_client_t* client)
{
    csilk_io_read_stop((csilk_io_stream_t*)&client->handle);
}
