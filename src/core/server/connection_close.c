/**
 * @file connection_close.c
 * @brief Connection close and teardown logic.
 */

#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

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
void
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
void
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
 *  @param client The client connection to destroy (must not be used after).
 */
void
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
        extern void pool_put_arena(worker_pool_t * wp, csilk_arena_t * arena);
        pool_put_arena(client->owner_pool, client->ctx.arena);
    }
    extern void pool_put(worker_pool_t * wp, csilk_client_t * client);
    pool_put(client->owner_pool, client);
}

/** @brief Get the I/O event loop associated with the request context.
 *
 *  Extracts the loop from the internal client's TCP handle.  Falls back to
 *  csilk_io_default_loop() if the context chain is incomplete (e.g. during early
 *  initialization or in unit tests).
 *
 *  @param c The request context.
 *  @return A pointer to the owning I/O event loop (never NULL).
 */
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
 *  @param c The request context.
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

/** @brief Decrement the async reference counter; destroy client if last ref.
 *
 *  When async_ref reaches 0 AND close_pending is 0 AND the connection is
 *  already marked closed (conn_closed), the client is fully destroyed.
 *  This prevents both leaks (abandoned clients) and use-after-free (relying
 *  on just one condition).
 *
 *  @param c The request context.
 */
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
                timers[i]->close_cb = on_timer_close;
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
