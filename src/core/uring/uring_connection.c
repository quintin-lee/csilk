/**
 * @file uring_connection.c
 * @brief Connection pool, accept, I/O, timers, and lifecycle callbacks using io_uring.
 */

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "core/internal/srv_internal.h"
#include "core/ctx/ctx_internal.h"
#include "../http/h2.h"
#include "../internal/srv_impl.h"
#include <limits.h>
#include <assert.h>
#include "uring_internal.h"

void csilk_client_close(csilk_client_t* client);

/* Forward declaration from http1.c */
static void on_sendfile_complete(csilk_io_fs_t* req);

/* --- Connection pool (per-worker, lock-free) --- */

/**
 * @brief Acquire a client object from the per-worker pool (or allocate one).
 * @param[in] wp Worker pool to draw from.
 * @return A recycled or freshly calloc'd client with file_fd reset to -1.
 * @note Prefers a pooled client; otherwise calloc's a new one. Caller is
 *       responsible for full initialization.
 */
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

/**
 * @brief Return a client to the per-worker pool after teardown.
 * @param[in] wp     Worker pool to return the client to.
 * @param[in] client Client to recycle (its SSL/H2 state is freed first).
 * @note Frees the SSL session and H2 streams, zeroes the struct (preserving the
 *       generation counter, which is incremented), then either pools or frees it.
 */
static void
pool_put(worker_pool_t* wp, csilk_client_t* client)
{
    if (client->ssl) {
        SSL_free(client->ssl);
        client->ssl = NULL;
        client->read_bio = NULL;
        client->write_bio = NULL;
    }
    if (client->h2_session) {
        nghttp2_session_del(client->h2_session);
        client->h2_session = NULL;
    }
    csilk_h2_free_streams(client);
    uint8_t old_gen = client->generation;
    memset(client, 0, sizeof(*client));
    client->generation = old_gen + 1;
    if (wp->client_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->client_pool[wp->client_pool_count++] = client;
    } else {
        free(client);
    }
}

/**
 * @brief Acquire an arena from the per-worker arena pool (or allocate one).
 * @param[in] wp Worker pool to draw from.
 * @return A recycled or new arena; new arenas honor the server's arena-alignment
 *         config when set.
 * @note Prefers a pooled arena; otherwise creates a CSILK_DEFAULT_ARENA_SIZE
 *       arena (with alignment applied if configured).
 */
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

/**
 * @brief Return an arena to the per-worker pool after resetting it.
 * @param[in] wp    Worker pool to return the arena to.
 * @param[in] arena Arena to recycle (reset to empty first).
 * @note Resets the arena, then either pools it (up to CSILK_CLIENT_POOL_SIZE) or
 *       frees it.
 */
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

/**
 * @brief Pre-populate a worker's arena pool at startup.
 * @param[in] wp Worker pool whose arena pool is filled.
 * @note Creates up to CSILK_CLIENT_POOL_SIZE default-sized arenas (applying
 *       alignment when configured), probing each with a 1-byte alloc/reset to
 *       validate, and stops on the first allocation failure.
 */
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
        void* p = csilk_arena_alloc(a, 1);
        if (!p) {
            csilk_arena_free(a);
            break;
        }
        csilk_arena_reset(a);
        wp->arena_pool[wp->arena_pool_count++] = a;
    }
}

/**
 * @brief Add a client to its owner worker's active-client linked list.
 *
 * NOT thread-safe: MUST be called exclusively on the client's owning worker
 * event-loop thread. Cross-thread operations must be dispatched via csilk_dispatch().
 *
 * @param[in] server Server (currently unused; the list is per-worker).
 * @param[in] client Client to insert at the head of the active list.
 */
static void
client_list_add(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
    worker_pool_t* wp = client->owner_pool;
    client->next = wp->active_clients;
    client->prev = NULL;
    if (wp->active_clients) {
        wp->active_clients->prev = client;
    }
    wp->active_clients = client;
}

/**
 * @brief Unlink a client from its owner worker's active-client list.
 *
 * NOT thread-safe: MUST be called exclusively on the client's owning worker
 * event-loop thread. Cross-thread operations must be dispatched via csilk_dispatch().
 *
 * @param[in] server Server owning the client (validated non-NULL).
 * @param[in] client Client to remove.
 */
static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
    if (!server) {
        return;
    }
    worker_pool_t* wp = client->owner_pool;
    if (!wp) {
        return;
    }
    if (client->prev) {
        client->prev->next = client->next;
    } else if (wp->active_clients == client) {
        wp->active_clients = client->next;
    }
    if (client->next) {
        client->next->prev = client->prev;
    }
    client->next = client->prev = NULL;
}

/**
 * @brief Fully tear down a client connection and recycle its resources.
 * @param[in] client Client to destroy.
 * @note Decrements the server's active-connection count, removes the client from
 *       the active list, cleans up its context, closes its fd, returns its arena
 *       to the pool, frees its read buffer, and returns the client to the pool.
 */
void
client_destroy(csilk_client_t* client)
{
    CSILK_LOG_D("client_destroy called, closing fd %d", client->handle.fd);
    if (client->server) {
        atomic_fetch_sub(&client->server->active_connections, 1);
        client_list_remove(client->server, client);
    }

    csilk_ctx_cleanup(&client->ctx);
    if (client->handle.fd >= 0) {
        close(client->handle.fd);
        client->handle.fd = -1;
    }
    if (client->ctx.arena) {
        pool_put_arena(client->owner_pool, client->ctx.arena);
    }
    if (client->read_buf) {
        free(client->read_buf);
        client->read_buf = NULL;
    }
    pool_put(client->owner_pool, client);
}

/**
 * @brief Resolve the io_uring event loop owning a request context.
 * @param[in] c Request context whose internal client identifies the worker.
 * @return The worker pool's loop pointer, or NULL if c/server/client is invalid.
 */
CSILK_INTERNAL csilk_io_loop_t*
_csilk_ctx_loop(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return NULL;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    return client->owner_pool->loop_ptr;
}

/**
 * @brief Increment a client's asynchronous-reference count.
 * @param[in] c Request context whose client tracks in-flight async ops.
 * @note No-op if c/server/internal client is invalid. Prevents the client from
 *       being destroyed while async work is outstanding.
 */
CSILK_INTERNAL void
_csilk_ctx_async_ref_incr(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    client->async_ref++;
}

/**
 * @brief Decrement a client's asynchronous-reference count.
 * @param[in] c Request context whose client tracks in-flight async ops.
 * @note No-op if invalid. When the ref count hits zero, no close is pending, and
 *       the connection is closed, the client is destroyed.
 */
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

/* --- I/O Operations --- */

/**
 * @brief Submit an io_uring timeout SQE for a client timer.
 * @param[in] client      Client owning the timer; its loop provides the ring.
 * @param[in] tmr         Timer handle whose data receives a heap-allocated timespec.
 * @param[in] timeout_ms  Timer duration in milliseconds.
 * @param[in] op          io_uring opcode used to tag the completion.
 * @note Allocates a timespec (freeing any prior one), prepares a one-shot timeout,
 *       and tags the SQE with the client pointer and op via uring_encode_data.
 */
static void
submit_timer(csilk_client_t* client, csilk_io_timer_t* tmr, uint64_t timeout_ms, uring_op_type_t op)
{
    struct io_uring*     ring = client->owner_pool->loop_ptr;
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
    if (!sqe) {
        return;
    }

    if (tmr->data) {
        free(tmr->data);
    }
    struct __kernel_timespec* ts = malloc(sizeof(struct __kernel_timespec));
    ts->tv_sec = timeout_ms / 1000;
    ts->tv_nsec = (timeout_ms % 1000) * 1000000;
    tmr->data = ts;

    io_uring_prep_timeout(sqe, ts, 0, 0);
    io_uring_sqe_set_data(sqe, (void*)uring_encode_data(op, client, client));
}

/**
 * @brief Begin asynchronous reading on a client connection via io_uring recv.
 * @param[in] client Client to start reading (validated; ignores duplicate starts).
 * @note Allocates a 64KB read buffer if needed, prepares a recv SQE tagged
 *       URING_OP_READ, submits it, and marks read_active on success.
 */
void
csilk_client_read_start(csilk_client_t* client)
{
    if (client->read_active) {
        return;
    }
    struct io_uring*     ring = client->owner_pool->loop_ptr;
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
    if (!sqe) {
        CSILK_LOG_E("Failed to get SQE for read!");
        return;
    }

    size_t suggested_size = 65536;
    if (client->read_buf == NULL) {
        client->read_buf = malloc(suggested_size);
    }
    void* buf = client->read_buf;

    io_uring_prep_recv(sqe, client->handle.fd, buf, suggested_size, 0);
    io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_READ, client, client));
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        CSILK_LOG_E("Failed to submit read! ret=%d", ret);
    } else {
        client->read_active = 1;
    }
}

/**
 * @brief Write data to a client connection (TLS-aware) via io_uring send.
 * @param[in] client Client to write to (no-op if NULL or connection closed).
 * @param[in] data   Bytes to send.
 * @param[in] length Number of bytes in data.
 * @note For TLS clients the data is SSL_write'd and flushed through the BIO.
 *       Otherwise a uring_write_req_t carrying a copy of the data is allocated,
 *       a send SQE is prepared (retrying for an SQE up to 100 times), tagged
 *       URING_OP_WRITE, and submitted with an incremented async_ref.
 */
void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t length)
{
    if (!client || client->ctx.conn_closed) {
        return;
    }

    if (client->ssl) {
        assert(length <= INT_MAX);
        SSL_write(client->ssl, data, (int)length);
        flush_tls_write(client);
        return;
    }

    uring_write_req_t* req = malloc(sizeof(uring_write_req_t) + length);
    if (!req) {
        return;
    }
    req->client = client;
    req->len = length;
    memcpy(req->data, data, length);

    struct io_uring*     ring = client->owner_pool->loop_ptr;
    struct io_uring_sqe* sqe = NULL;
    int                  retries = 0;
    while (!sqe && retries < 100) {
        sqe = uring_get_sqe_or_submit(ring);
        if (!sqe) {
            retries++;
            /* Brief pause to let the kernel drain in-flight SQEs */
            usleep(10);
        }
    }
    if (!sqe) {
        CSILK_LOG_E("csilk_client_write: cannot get SQE after %d retries — "
                    "response dropped!",
                    retries);
        free(req);
        return;
    }

    io_uring_prep_send(sqe, client->handle.fd, req->data, length, 0);
    io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_WRITE, client, req));
    atomic_fetch_add(&client->async_ref, 1);
    io_uring_submit(ring);
}

/**
 * @brief Completion handler for a client send SQE.
 * @param[in] arg The uring_write_req_t carrying the client and data.
 * @param[in] res Bytes written, or a negative error.
 * @note Frees the write request. On error closes the client; on success, if a
 *       file is pending, issues a sendfile; otherwise stops the write timer and
 *       decrements the async ref (destroying the client when it hits zero and
 *       keep-alive/finalization allows).
 */
void
on_write_done(void* arg, ssize_t res)
{
    uring_write_req_t* req = (uring_write_req_t*)arg;
    if (!req) {
        return;
    }
    csilk_client_t* client = req->client;
    /* req->data is the flexible array member, embedded in req */
    free(req);

    CSILK_LOG_D("on_write_done: res=%zd", res);
    if (res < 0) {
        if (res != -ECONNRESET && res != -EPIPE) {
            CSILK_LOG_E("Connection: write error %zd", res);
        }
        csilk_client_close(client);
    } else if (client->ctx.file_fd >= 0 && !client->ssl) {
        int    fd = client->ctx.file_fd;
        size_t offset = client->ctx.file_offset;
        size_t size = client->ctx.file_size;
        client->ctx.file_fd = -1;

        CSILK_LOG_D("Sendfile: fd=%d offset=%zu size=%zu", fd, offset, size);

        csilk_io_fs_t* fs_req = malloc(sizeof(csilk_io_fs_t));
        if (fs_req) {
            fs_req->data = &client->ctx;
            int r = csilk_io_fs_sendfile(
                NULL, fs_req, client->handle.fd, fd, offset, size, on_sendfile_complete);
            if (r >= 0) {
                return;
            }
            free(fs_req);
        }
    }

    int keep_alive = client->keep_alive;
    if (client->server->config.write_timeout_ms > 0) {
        csilk_io_timer_stop(&client->write_timer);
    }

    int outstanding = atomic_fetch_sub(&client->async_ref, 1) - 1;

    _csilk_handle_post_response(client, keep_alive);

    if (outstanding == 0 && client->close_pending == 0 && client->ctx.conn_closed) {
        client_destroy(client);
    }
}

/* --- Sendfile completion callback (uring backend) --- */
/**
 * @brief Completion callback for a client sendfile transfer.
 * @param[in] req The fs request whose data points at the client context.
 * @note Frees the fs request, stops the write timer, decrements the async ref,
 *       finalizes the response, and destroys the client when all references are
 *       released and the connection is closed.
 */
static void
on_sendfile_complete(csilk_io_fs_t* req)
{
    if (!req) {
        return;
    }
    csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    free(req);

    if (!client) {
        return;
    }

    int keep_alive = client->keep_alive;

    if (client->server->config.write_timeout_ms > 0) {
        csilk_io_timer_stop(&client->write_timer);
    }

    int outstanding = atomic_fetch_sub(&client->async_ref, 1) - 1;

    CSILK_LOG_D("Sendfile complete: keep_alive=%d outstanding=%d", keep_alive, outstanding);
    _csilk_handle_post_response(client, keep_alive);

    if (outstanding == 0 && client->close_pending == 0 && client->ctx.conn_closed) {
        client_destroy(client);
    }
}

/**
 * @brief Callback invoked when a client close has fully completed.
 * @param[in] client Client whose close finished.
 * @note Decrements close_pending; when close_pending and async_ref are both
 *       zero and the connection is closed, the client is destroyed.
 */
void
on_close_done(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    if (client->close_pending > 0) {
        client->close_pending--;
    }
    CSILK_LOG_D("on_close_done: close_pending=%d async_ref=%d",
                client->close_pending,
                atomic_load(&client->async_ref));
    if (client->close_pending == 0 && atomic_load(&client->async_ref) <= 0 &&
        client->ctx.conn_closed) {
        client_destroy(client);
    }
}

/**
 * @brief Initiate graceful closure of a client connection.
 * @param[in] client Client to close (no-op if NULL or already closed).
 * @note Fires the connection-close hook, removes the client from the active list,
 *       marks the context closed, frees the timer timespecs, and submits io_uring
 *       cancel SQEs for pending reads/timeouts (writes are allowed to drain).
 *       Destroys the client immediately if nothing is pending.
 */
void
csilk_client_close(csilk_client_t* client)
{
    if (!client || client->ctx.conn_closed) {
        return;
    }
    CSILK_LOG_D("csilk_client_close called");

    CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
    client_list_remove(client->server, client);
    client->ctx.conn_closed = 1;

    if (client->timer.data) {
        free(client->timer.data);
        client->timer.data = NULL;
    }
    if (client->read_timer.data) {
        free(client->read_timer.data);
        client->read_timer.data = NULL;
    }
    if (client->write_timer.data) {
        free(client->write_timer.data);
        client->write_timer.data = NULL;
    }
    if (client->request_timer.data) {
        free(client->request_timer.data);
        client->request_timer.data = NULL;
    }

    struct io_uring* ring = client->owner_pool->loop_ptr;

    client->close_pending = 0;
    // Cancel pending reads/timeouts (do not cancel writes, allow them to drain)
    uring_op_type_t ops_to_cancel[] = {
        URING_OP_READ, URING_OP_TMR_READ, URING_OP_TMR_WRITE, URING_OP_TMR_IDLE, URING_OP_TMR_REQ};
    for (int i = 0; i < 5; i++) {
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
        if (!sqe) {
            continue;
        }
        io_uring_prep_cancel(sqe, (void*)uring_encode_data(ops_to_cancel[i], client, client), 0);
        io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_CLOSE, client, client));
        client->close_pending++;
    }

    io_uring_submit(ring);

    if (client->close_pending == 0 && client->async_ref <= 0) {
        client_destroy(client);
    }
}

/**
 * @brief Timer expiry callback that closes a timed-out connection.
 * @param[in] client Client whose timer fired.
 * @note If the connection is not already closed, initiates csilk_client_close.
 */
void
on_timeout(csilk_client_t* client)
{
    if (!client->ctx.conn_closed) {
        CSILK_LOG_D("Connection: closing connection due to timeout");
        csilk_client_close(client);
    }
}

/**
 * @brief Accept and initialize a new client connection on a worker.
 * @param[in] wp        Worker pool that accepted the connection.
 * @param[in] client_fd Accepted socket fd.
 * @note Enforces the max-connections limit (closing the fd if exceeded), pulls a
 *       client from the pool, sets up its context/arena/parser, fires the
 *       connection-open hook, initializes TLS if configured, arms read/request
 *       timers, and starts reading.
 */
void
on_new_connection(worker_pool_t* wp, int client_fd)
{
    csilk_server_t* server = wp->server;

    if (_csilk_server_try_acquire_connection(server) < 0) {
        close(client_fd);
        return;
    }

    csilk_client_t* client = pool_get(wp);
    if (!client) {
        _csilk_server_release_connection(server);
        close(client_fd);
        return;
    }

    client->server = server;
    client->owner_pool = wp;
    client->handle.fd = client_fd;
    client->handle.data = client;

    _csilk_ctx_init(&client->ctx, server, client);
    client->ctx.arena = pool_get_arena(wp);

    client_list_add(server, client);

    CSILK_LOG_D("Worker %d: accepted new TCP connection (fd: %d, client pointer: %p)",
                wp->worker_index,
                client_fd,
                (void*)client);

    client->protocol = CSILK_PROTO_HTTP1;
    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    client->parser.data = client;

    _csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

    if (server->ssl_ctx) {
        CSILK_LOG_D("Connection: setting up TLS for connection: %p", (void*)client);
        if (setup_client_tls(client) < 0) {
            csilk_client_close(client);
            return;
        }
    }

    CSILK_LOG_T("Connection: connection timers initialized, starting read listener");
    if (server->config.read_timeout_ms > 0) {
        submit_timer(
            client, &client->read_timer, server->config.read_timeout_ms, URING_OP_TMR_READ);
    }
    if (server->config.request_timeout_ms > 0) {
        submit_timer(
            client, &client->request_timer, server->config.request_timeout_ms, URING_OP_TMR_REQ);
    }

    if (!client->read_paused) {
        csilk_client_read_start(client);
    }

    struct io_uring* ring = client->owner_pool->loop_ptr;
    io_uring_submit(ring);
}

/**
 * @brief Handle a completed recv on a client connection.
 * @param[in] client Client that produced the read.
 * @param[in] nread  Bytes received, 0 for EOF, or a negative error.
 * @note Resets read state, re-arms the read timer, then routes the data to TLS
 *       processing, WebSocket frame parsing, or HTTP (llhttp) parsing. On
 *       success re-arms the next read; on EOF/error closes the client.
 */
void
on_read(csilk_client_t* client, ssize_t nread)
{
    CSILK_LOG_D("Worker: on_read nread=%zd (client=%p)", nread, (void*)client);
    if (!client || client->ctx.conn_closed) {
        return;
    }

    char* base = client->read_buf;
    client->read_buf = NULL;
    client->read_active = 0;
    int is_registered = 0;

    if (client->server->config.read_timeout_ms > 0) {
        submit_timer(
            client, &client->read_timer, client->server->config.read_timeout_ms, URING_OP_TMR_READ);
    }

    if (nread > 0) {
        if (client->ssl) {
            BIO_write(client->read_bio, base, (int)nread);
            process_tls_read(client);
        } else if (client->ctx.is_websocket) {
            csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
        } else {
            if (_csilk_ctx_register_read_buffer(&client->ctx, base) == 0) {
                is_registered = 1;
            } else {
                CSILK_LOG_E("Connection: failed to register read buffer, out of memory");
            }

            enum llhttp_errno err = llhttp_execute(&client->parser, base, nread);
            if (err == HPE_CLOSED_CONNECTION) {
                llhttp_init(&client->parser, HTTP_REQUEST, &client->server->settings);
                client->parser.data = client;
            } else if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
                CSILK_LOG_E("Connection: HTTP parse error: %s %s",
                            llhttp_errno_name(err),
                            client->parser.reason ? client->parser.reason : "unknown reason");
                csilk_client_close(client);
            }
        }

    } else if (nread < 0) {
        if (nread != -ECONNRESET) {
            CSILK_LOG_E("Connection: read error %zd", nread);
        }
        csilk_client_close(client);
    } else {
        csilk_client_close(client);
    }

    if (base && !is_registered) {
        free(base);
    }

    if (!client->ctx.conn_closed && nread > 0) {
        if (!client->read_paused) {
            csilk_client_read_start(client);
        }
    }

    if (!client->ctx.conn_closed) {
        struct io_uring* ring = client->owner_pool->loop_ptr;
        io_uring_submit(ring);
    }
}

/**
 * @brief Resolve the remote IP address of a client context.
 * @param[in] c Request context whose internal client carries the socket.
 * @return A heap/arena-duplicated string with the peer IP (IPv4 or IPv6), or
 *         NULL if the context is invalid or getpeername fails.
 * @note The returned string is allocated from the context arena.
 */
const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return NULL;
    }
    csilk_client_t*         client = (csilk_client_t*)c->_internal_client;
    struct sockaddr_storage addr;
    socklen_t               len = sizeof(addr);
    if (getpeername(client->handle.fd, (struct sockaddr*)&addr, &len) == 0) {
        char ip[46];
        if (addr.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, ip, sizeof(ip));
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, ip, sizeof(ip));
        }
        return csilk_arena_strdup(c->arena, ip);
    }
    return NULL;
}

/**
 * @brief Pause further reads on a client connection.
 * @param[in] client Client whose reads are paused.
 * @note Sets read_paused so the read loop does not re-arm after the next read.
 */
void
csilk_client_read_stop(csilk_client_t* client)
{
    client->read_paused = 1;
}
