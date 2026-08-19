/**
 * @file connection_close.c
 * @brief Connection lifetime, reference counting, close, and teardown logic.
 */

#include <stdatomic.h>
#include <unistd.h>
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
    if (!wp) {
        return;
    }
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

/* --- Connection Lifetime & Reference Subsystem --- */

/**
 * @brief Increment the connection reference count.
 * @param client The client connection.
 * @return The new reference count.
 */
int
csilk_client_ref(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
    return atomic_fetch_add(&client->ref_count, 1) + 1;
}

/**
 * @brief Decrement the connection reference count and recycle if eligible.
 * @param client The client connection.
 * @return The new reference count.
 */
int
csilk_client_unref(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
    int prev = atomic_fetch_sub(&client->ref_count, 1);
    int curr = prev - 1;
    if (curr <= 0) {
        _csilk_client_check_recycle(client);
    }
    return curr;
}

/**
 * @brief Increment active pending I/O operations (writes, timer closes).
 * @param client The client connection.
 * @return The new pending I/O count.
 */
int
_csilk_client_pending_io_inc(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
    return atomic_fetch_add(&client->pending_io, 1) + 1;
}

/**
 * @brief Decrement active pending I/O operations and recycle if eligible.
 * @param client The client connection.
 * @return The new pending I/O count.
 */
int
_csilk_client_pending_io_dec(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
    int prev = atomic_fetch_sub(&client->pending_io, 1);
    int curr = prev - 1;
    if (curr <= 0) {
        _csilk_client_check_recycle(client);
    }
    return curr;
}

/**
 * @brief Check whether client is eligible for recycling and recycle if so.
 *
 * Invariant condition:
 *   (state == CLOSING || state == CLOSED)
 *   && ref_count == 0
 *   && pending_io == 0
 *
 * @param client The client connection.
 */
void
_csilk_client_check_recycle(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t st = client->state;
    if ((st == CSILK_CONN_CLOSING || st == CSILK_CONN_CLOSED) &&
        atomic_load(&client->ref_count) <= 0 && atomic_load(&client->pending_io) <= 0) {
        client_destroy(client);
    }
}

/**
 * @brief Teardown client context, arena, and return to worker-local pool.
 * @param client The client connection to destroy.
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
        client->ctx.arena = NULL;
    }
    extern void pool_put(worker_pool_t * wp, csilk_client_t * client);
    pool_put(client->owner_pool, client);
}

/** @brief Get the I/O event loop associated with the request context. */
CSILK_INTERNAL csilk_io_loop_t*
_csilk_ctx_loop(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return csilk_io_default_loop();
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    return client->handle.loop;
}

/** @brief Context async lease helper — increments client ref_count. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_incr(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return;
    }
    csilk_client_ref((csilk_client_t*)c->_internal_client);
}

/** @brief Context async lease helper — decrements client ref_count. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_decr(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return;
    }
    csilk_client_unref((csilk_client_t*)c->_internal_client);
}

/* --- Connection close --- */

/**
 * @brief Close callback for client TCP handles — initiates graceful teardown.
 *
 * Marks state as CLOSING, unlinks from active list, stops all timers,
 * initiates timer closures with pending_io tracking, and releases the base
 * connection reference held since accept.
 *
 * @param handle The TCP handle being closed (data points to csilk_client_t).
 */
CSILK_INTERNAL void
on_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!client) {
        return;
    }

    CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
    csilk_conn_set_state(client, CSILK_CONN_CLOSING);
    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
    client_list_remove(client->server, client);
    client->ctx.conn_closed = 1;

    csilk_io_timer_stop(&client->timer);
    csilk_io_timer_stop(&client->read_timer);
    csilk_io_timer_stop(&client->write_timer);
    csilk_io_timer_stop(&client->request_timer);

    csilk_io_handle_t* timers[] = {(csilk_io_handle_t*)&client->timer,
                                   (csilk_io_handle_t*)&client->read_timer,
                                   (csilk_io_handle_t*)&client->write_timer,
                                   (csilk_io_handle_t*)&client->request_timer};
    for (int i = 0; i < 4; i++) {
        if (!csilk_io_is_closing(timers[i])) {
            _csilk_client_pending_io_inc(client);
            timers[i]->data = client;
            timers[i]->close_cb = on_timer_close;
            csilk_io_close(timers[i], on_timer_close);
        }
    }

    /* Release base connection reference held since on_new_connection */
    csilk_client_unref(client);
}
