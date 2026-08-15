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
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../http/h2.h"
#include "../internal/srv_impl.h"

/* --- Buffer allocation --- */

/** @brief Forward declaration for pool_get_read_buf. */
static void pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf);

/** @brief I/O buffer allocation callback — allocates a receive buffer.
 *
 * Allocates a buffer of the suggested size using malloc. The buffer is freed
 * by the I/O backend after the read callback is invoked (libuv path).
 *
 * // @param handle          The I/O handle that will read into the buffer.
 * @param suggested_size  Recommended buffer size from the I/O backend.
 * @param buf             [out] Pointer to the csilk_io_buf_t to populate. */
void
alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf)
{
    worker_pool_t* wp = NULL;
    if (handle && handle->data) {
        csilk_client_t* client = (csilk_client_t*)handle->data;
        wp = client->owner_pool;
    }
    pool_get_read_buf(wp, suggested_size, buf);
}

/* --- Connection lifecycle state machine --- */

/**
 * @brief Get human-readable string representation of connection state.
 * @param state Connection lifecycle state.
 * @return Static string description of the state.
 */
const char*
csilk_conn_state_str(csilk_conn_state_t state)
{
    switch (state) {
    case CSILK_CONN_INIT:
        return "INIT";
    case CSILK_CONN_ACCEPTED:
        return "ACCEPTED";
    case CSILK_CONN_TLS:
        return "TLS";
    case CSILK_CONN_READING:
        return "READING";
    case CSILK_CONN_PROCESSING:
        return "PROCESSING";
    case CSILK_CONN_WRITING:
        return "WRITING";
    case CSILK_CONN_STREAMING:
        return "STREAMING";
    case CSILK_CONN_CLOSING:
        return "CLOSING";
    case CSILK_CONN_CLOSED:
        return "CLOSED";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Transition a connection to a new lifecycle state with invariant validation.
 * @param client    Client connection instance.
 * @param new_state Target lifecycle state.
 */
void
csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t old_state = client->state;
    if (old_state == new_state) {
        return;
    }

    /* Invariant: once CLOSED, connection cannot transition except to INIT (pool reuse) */
    if (old_state == CSILK_CONN_CLOSED && new_state != CSILK_CONN_INIT) {
        CSILK_LOG_W("Conn %p: illegal state transition from CLOSED to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    /* Invariant: once CLOSING, only allowed next state is CLOSED */
    if (old_state == CSILK_CONN_CLOSING && new_state != CSILK_CONN_CLOSED) {
        CSILK_LOG_D("Conn %p: ignored transition from CLOSING to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    CSILK_LOG_T("Conn %p state: %s -> %s",
                (void*)client,
                csilk_conn_state_str(old_state),
                csilk_conn_state_str(new_state));
    client->state = new_state;
}

/**
 * @brief Get the current lifecycle state of a connection.
 * @param client Client connection instance.
 * @return Current connection state, or CSILK_CONN_CLOSED if client is NULL.
 */
csilk_conn_state_t
csilk_conn_get_state(const csilk_client_t* client)
{
    return client ? client->state : CSILK_CONN_CLOSED;
}

/* --- Connection pool (per-worker, lock-free) --- */

/** @brief Get a client connection object from the worker-local free pool or
 *  allocate a new one.
 *
 * In multi-worker mode, each worker thread has its own pool with no shared
 * state — pool_get is a pure thread-local O(1) operation with zero locking.
 *
 * @param wp The worker pool (must not be NULL).
 * @return A csilk_client_t ready for use, or NULL on allocation failure. */
static csilk_client_t*
pool_get(worker_pool_t* wp)
{
    csilk_client_t* client;
    if (wp && wp->client_pool_count > 0) {
        client = wp->client_pool[--wp->client_pool_count];
    } else {
        client = calloc(1, sizeof(csilk_client_t));
    }
    if (client) {
        uint8_t gen = (uint8_t)((client->generation + 1) & 0xFF);
        if (gen == 0) {
            gen = 1;
        }
        client->state = CSILK_CONN_INIT;
        client->generation = gen;
#ifdef CSILK_USE_URING
        client->handle.generation = gen;
        client->timer.generation = gen;
        client->read_timer.generation = gen;
        client->write_timer.generation = gen;
        client->request_timer.generation = gen;
        /* A struct recycled while its previous incarnation is still tearing
         * down (async write CQE pending) carries stale teardown state; start
         * pristine so a stale write completion for the old incarnation cannot
         * trigger destruction of this connection. */
        client->async_ref = 0;
        client->close_pending = 0;
        client->ctx.conn_closed = 0;
#endif
        client->ctx.file_fd = -1;
    }
    return client;
}

/** @brief Reset only the mutable/hot request and connection fields during recycling.
 *
 * Avoids expensive full-struct memset (which blows CPU caches and clears
 * immutable handle memory) while preserving generation counters and pre-allocated
 * I/O buffers.
 *
 * @param client The client connection to reset. */
static void
reset_hot_state(csilk_client_t* client)
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

    /* Connection lifecycle and parser flags */
    client->state = CSILK_CONN_INIT;
    client->close_pending = 0;
    client->async_ref = 0;
    client->read_paused = 0;
    client->read_active = 0;
    client->keep_alive = 0;
    client->pending_write_bytes = 0;
    client->protocol = CSILK_PROTO_HTTP1;
    client->total_header_size = 0;
    client->header_count = 0;
    client->current_url.data = NULL;
    client->current_url.len = 0;
    client->current_header_field.data = NULL;
    client->current_header_field.len = 0;
    client->current_header_value.data = NULL;
    client->current_header_value.len = 0;
    client->next = NULL;
    client->prev = NULL;
    client->server = NULL;
    client->owner_pool = NULL;

    /* Context mutable state */
    client->ctx.handler_index = -1;
    client->ctx.handlers = NULL;
    client->ctx.handler_count = 0;
    client->ctx.aborted = 0;
    client->ctx.panicked = 0;
    client->ctx.defer_head = NULL;
    client->ctx.params_count = 0;
    client->ctx.is_websocket = 0;
    client->ctx.is_sse = 0;
    client->ctx.is_async = 0;
    client->ctx.response_started = 0;
    client->ctx.write_paused = 0;
    client->ctx.on_drain = NULL;
    client->ctx.on_drain_data = NULL;
    client->ctx.file_fd = -1;
    client->ctx.file_offset = 0;
    client->ctx.file_size = 0;
    client->ctx.storage_head = NULL;
    client->ctx.stream_id = 0;
    client->ctx.next_stream = NULL;
    client->ctx.conn_closed = 0;
    client->ctx.on_ws_message = NULL;
    client->ctx.on_ws_send = NULL;
    client->ctx.read_buffers = client->ctx.read_buffers_embedded;
    client->ctx.read_buffers_count = 0;
    client->ctx.read_buffers_capacity = 16;
    memset(client->ctx.request_id, 0, sizeof(client->ctx.request_id));
}

/** @brief Return a client to the worker-local free pool for reuse.
 *
 * SSL and H2 sessions are cleaned before returning. Only hot mutable state is
 * reset via reset_hot_state() to avoid cache eviction from full struct zeroing.
 * If the pool has room, the client is saved for reuse; otherwise freed. No lock
 * — only the owning event-loop thread accesses this.
 *
 * @param wp     The worker pool (derived from client->owner_pool).
 * @param client The client to return (must not be used after this call). */
static void
pool_put(worker_pool_t* wp, csilk_client_t* client)
{
    if (!client) {
        return;
    }
    reset_hot_state(client);
    if (wp && wp->client_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->client_pool[wp->client_pool_count++] = client;
    } else {
#ifdef CSILK_USE_URING
        if (client->read_buf) {
            pool_put_read_buf(wp, (char*)client->read_buf, CSILK_READ_BUF_64KB);
            client->read_buf = NULL;
        }
#endif
        free(client);
    }
}

/** @brief Get a pre-allocated arena from the worker-local arena pool.
 *
 * Pops a pre-allocated arena from the pool. If the pool is empty, falls back
 * to creating a new arena on the fly. Pre-allocated arenas already have their
 * first chunk ready, so the hot path avoids aligned_alloc entirely.
 *
 * @param wp The worker pool (must not be NULL).
 * @return A csilk_arena_t ready for use, or NULL on allocation failure. */
static csilk_arena_t*
pool_get_arena(worker_pool_t* wp)
{
    if (!wp) {
        return csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    }
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
    if (!arena) {
        return;
    }
    csilk_arena_reset(arena);
    if (wp && wp->arena_pool_count < CSILK_CLIENT_POOL_SIZE) {
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

/* --- Read buffer pool (three-tier: 4KB / 16KB / 64KB) --- */

/** @brief Select the tier index for a given buffer size.
 *
 * Returns the smallest tier whose capacity is >= suggested_size.
 * Falls back to the largest tier if no smaller tier fits.
 *
 * @param suggested_size Requested buffer size in bytes.
 * @return Tier index (0 = 4KB, 1 = 16KB, 2 = 64KB). */
static int
read_buf_tier_index(size_t suggested_size)
{
    if (suggested_size <= CSILK_READ_BUF_4KB) {
        return 0;
    }
    if (suggested_size <= CSILK_READ_BUF_16KB) {
        return 1;
    }
    return 2;
}

/** @brief Get a read buffer from the worker-local pool.
 *
 * Pops a buffer from the tier matching suggested_size. If the tier is empty,
 * falls back to malloc. The buffer is returned via @p buf.
 *
 * @param wp             The worker pool (must not be NULL).
 * @param suggested_size Requested buffer size.
 * @param buf            [out] Populated with base pointer and length. */
static void
pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf)
{
    int    tier = read_buf_tier_index(suggested_size);
    size_t tier_size;
    switch (tier) {
    case 0:
        tier_size = CSILK_READ_BUF_4KB;
        break;
    case 1:
        tier_size = CSILK_READ_BUF_16KB;
        break;
    default:
        tier_size = CSILK_READ_BUF_64KB;
        break;
    }

    if (wp && wp->read_buf_counts[tier] > 0) {
        buf->base = (char*)wp->read_buf_tiers[tier][--wp->read_buf_counts[tier]];
        buf->len = tier_size;
    } else {
        buf->base = (char*)malloc(tier_size);
        buf->len = tier_size;
    }
}

/** @brief Return a read buffer to the worker-local pool.
 *
 * Pushes the buffer back onto the appropriate tier's free list.
 * If the tier is full, the buffer is freed instead.
 *
 * @param wp     The worker pool (may be NULL — falls back to free).
 * @param base   Buffer pointer to return.
 * @param size   Buffer capacity (used to select the tier). */
void
pool_put_read_buf(worker_pool_t* wp, char* base, size_t size)
{
    if (!base || !wp) {
        free(base);
        return;
    }
    int tier = read_buf_tier_index(size);
    if (wp->read_buf_counts[tier] < CSILK_READ_BUF_POOL_SIZE) {
        wp->read_buf_tiers[tier][wp->read_buf_counts[tier]++] = (void*)base;
    } else {
        free(base);
    }
}

/** @brief Pre-allocate buffers for each tier of the read buffer pool.
 *
 * Called once per worker at startup. Each tier gets CSILK_READ_BUF_POOL_SIZE
 * pre-allocated buffers of the corresponding size.
 *
 * @param wp The worker pool to initialise (must not be NULL). */
void
_csilk_worker_init_read_buf_pool(worker_pool_t* wp)
{
    const size_t tier_sizes[] = {CSILK_READ_BUF_4KB, CSILK_READ_BUF_16KB, CSILK_READ_BUF_64KB};
    for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
        for (int i = 0; i < CSILK_READ_BUF_POOL_SIZE; i++) {
            void* p = malloc(tier_sizes[tier]);
            if (!p) {
                break;
            }
            wp->read_buf_tiers[tier][wp->read_buf_counts[tier]++] = p;
        }
    }
}

/* --- Active client list --- */

/**
 * @brief Insert a client at the head of the worker's active client list.
 *
 * NOT thread-safe: MUST be called exclusively on the client's owning worker
 * event-loop thread. Cross-thread operations must be dispatched via csilk_dispatch().
 *
 * @param server The server instance (unused; list is worker-local).
 * @param client The client to add (must not already be in a list).
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
 * @brief Remove a client from the worker's active client list.
 *
 * NOT thread-safe: MUST be called exclusively on the client's owning worker
 * event-loop thread. Cross-thread operations must be dispatched via csilk_dispatch().
 *
 * Unlinks the client from the doubly-linked list and clears its prev/next pointers.
 *
 * @param server The server instance (unused; list is worker-local).
 * @param client The client to remove.
 */
static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
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
    if (!client || client->state == CSILK_CONN_CLOSED) {
        return;
    }
#ifdef CSILK_USE_URING
    if (client->handle.fd >= 0) {
        close(client->handle.fd);
        client->handle.fd = -1;
    }
#endif
    if (client->server) {
        atomic_fetch_sub(&client->server->active_connections, 1);
    }
    csilk_conn_set_state(client, CSILK_CONN_CLOSED);
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
 *  @return A pointer to the owning I/O event loop (never NULL). */
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
    if (client->async_ref <= 0) {
#ifdef CSILK_USE_URING
        if ((client->handle.flags & CSILK_IO_HANDLE_CLOSING) && client->handle.fd >= 0) {
            close(client->handle.fd);
            client->handle.fd = -1;
        }
#endif
        if (client->close_pending <= 0 && c->conn_closed) {
            client_destroy(client);
        }
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
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);
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
        int                closed_count = 0;
        for (int i = 0; i < 4; i++) {
            if (csilk_io_is_closing(timers[i])) {
                client->close_pending--;
            } else {
                closed_count++;
                timers[i]->data = client;
                csilk_io_close(timers[i], on_timer_close);
            }
        }
        if (closed_count == 0 && client->close_pending <= 0) {
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
on_idle_timeout(csilk_io_timer_t* handle)
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
on_read_timeout(csilk_io_timer_t* handle)
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
on_write_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/* --- Rejected connection --- */

#ifndef CSILK_USE_URING
/** @brief Close callback for rejected (connection-limited) TCP handles.
 *
 *  When the server reaches max_connections, excess connections are accepted
 *  and immediately closed. The handle (a temporary csilk_io_tcp_t allocated in
 *  on_new_connection) is freed here. This drains the kernel TCP backlog
 *  without allocating a full csilk_client_t.
 *
 *  @param handle The temporary TCP handle to free. */
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
            csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
        } else {
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

    if (_csilk_server_try_acquire_connection(server) < 0) {
        reject_connection(server_stream);
        return;
    }

    csilk_client_t* client = pool_get(wp);
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
        pool_put(wp, client);
        return;
    }
    client->handle.data = client;

    _csilk_ctx_init(&client->ctx, server, client);
    client->ctx.arena = pool_get_arena(wp);

    client_list_add(server, client);

    if (csilk_io_accept(server_stream, (csilk_io_stream_t*)&client->handle) == 0) {
        CSILK_LOG_D("Connection: accepted new TCP connection (client pointer: %p)", (void*)client);
        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
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
            if (setup_client_tls(client) < 0) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
                return;
            }
        }

        csilk_io_timer_init(server_stream->loop, &client->timer);
        client->timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->read_timer);
        client->read_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->write_timer);
        client->write_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->request_timer);
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

        if (!server->ssl_ctx) {
            csilk_conn_set_state(client, CSILK_CONN_READING);
        }

        r = csilk_io_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
        if (r < 0) {
            CSILK_LOG_E("Connection: csilk_io_read_start error: %s", csilk_io_strerror(r));
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
    } else {
        client_list_remove(server, client);
        _csilk_server_release_connection(server);
        if (client->ctx.arena) {
            pool_put_arena(wp, client->ctx.arena);
            client->ctx.arena = NULL;
        }
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
 * @param buf    The buffer that was read into (freed by this callback). */
void
on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf)
{
    csilk_client_t* client = (csilk_client_t*)stream->data;
    char*           base = buf->base;
    int             is_registered = 0;

    if (!client || client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        if (base) {
            pool_put_read_buf(NULL, base, buf->len);
        }
        return;
    }

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
            csilk_conn_set_state(client, CSILK_CONN_STREAMING);
            csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
        } else {
            csilk_conn_set_state(client, CSILK_CONN_READING);

            /* Register the receive buffer so it stays alive for zero-copy header/body views. */
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

                if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                    csilk_io_close((csilk_io_handle_t*)stream, on_close);
                }
            }
        }

    } else if (nread < 0) {
        if (nread != -1 && nread != -4095 /* UV_EOF */) {
            CSILK_LOG_E("Connection: read error: %s", csilk_io_err_name((int)nread));
        }
        if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
            csilk_io_close((csilk_io_handle_t*)stream, on_close);
        }
    }

    if (base && !is_registered) {
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
 *         if the context is NULL or the address cannot be resolved. */
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
