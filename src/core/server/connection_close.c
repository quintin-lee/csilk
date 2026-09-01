/**
 * @file connection_close.c
 * @brief Connection lifetime, reference counting, close, and teardown logic.
 */

#include <assert.h>
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
    assert(_csilk_is_owner_worker_thread(wp) &&
           "client_list_add called from non-owner worker thread");
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
    assert(_csilk_is_owner_worker_thread(wp) &&
           "client_list_remove called from non-owner worker thread");
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

/* --- Thread-Local Worker Pool Identification --- */

static _Thread_local worker_pool_t* g_current_worker_pool = NULL;

typedef struct {
    csilk_io_handle_t* handle;
    uint64_t           generation;
} csilk_close_task_payload_t;

static void _csilk_on_close_owner(void* arg);
void        on_close(csilk_io_handle_t* handle);

void
_csilk_worker_set_current_pool(worker_pool_t* wp)
{
    g_current_worker_pool = wp;
}

worker_pool_t*
_csilk_worker_get_current_pool(void)
{
    return g_current_worker_pool;
}

/* Recycle dispatch task payload */
typedef struct {
    csilk_client_t* client;
    uint64_t        generation;
} csilk_recycle_task_payload_t;

static void
_csilk_client_recycle_dispatch_cb(void* arg)
{
    csilk_recycle_task_payload_t* p = (csilk_recycle_task_payload_t*)arg;
    if (!p) {
        return;
    }
    csilk_client_t* client = p->client;
    uint64_t        gen = p->generation;
    free(p);

    if (!client) {
        return;
    }
    /* Verify generation and closing state to eliminate ABA */
    if (client->generation == gen && client->state == CSILK_CONN_CLOSING) {
        if (atomic_load_explicit(&client->ref_count, memory_order_acquire) <= 0 &&
            atomic_load_explicit(&client->pending_io, memory_order_acquire) <= 0) {
            client_destroy(client);
        }
    }
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
    return atomic_fetch_add_explicit(&client->ref_count, 1, memory_order_acq_rel) + 1;
}

/**
 * @brief Decrement the connection reference count and recycle if eligible.
 * @param client The client connection.
 * @return The new reference count.
 */
int
csilk_client_unref(csilk_client_t* client)
{
    if (__builtin_expect(!client, 0)) {
        return 0;
    }
    int prev = atomic_fetch_sub_explicit(&client->ref_count, 1, memory_order_acq_rel);
    if (__builtin_expect(prev <= 0, 0)) {
        atomic_store_explicit(&client->ref_count, 0, memory_order_relaxed);
        return 0;
    }
    int curr = prev - 1;
    if (__builtin_expect(curr == 0, 0)) {
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
    if (__builtin_expect(!client, 0)) {
        return 0;
    }
    return atomic_fetch_add_explicit(&client->pending_io, 1, memory_order_acq_rel) + 1;
}

/**
 * @brief Decrement active pending I/O operations and recycle if eligible.
 * @param client The client connection.
 * @return The new pending I/O count.
 */
int
_csilk_client_pending_io_dec(csilk_client_t* client)
{
    if (__builtin_expect(!client, 0)) {
        return 0;
    }
    int prev = atomic_fetch_sub_explicit(&client->pending_io, 1, memory_order_acq_rel);
    if (__builtin_expect(prev <= 0, 0)) {
        atomic_store_explicit(&client->pending_io, 0, memory_order_relaxed);
        return 0;
    }
    int curr = prev - 1;
    if (__builtin_expect(curr == 0, 0)) {
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

    worker_pool_t* wp = client->owner_pool;

    /* Owner thread: check state and recycle directly (single-threaded access) */
    if (_csilk_is_owner_worker_thread(wp)) {
        if (client->state != CSILK_CONN_CLOSING && client->state != CSILK_CONN_CLOSED) {
            return;
        }
        int ref = atomic_load_explicit(&client->ref_count, memory_order_acquire);
        int pio = atomic_load_explicit(&client->pending_io, memory_order_acquire);
        if (ref <= 0 && pio <= 0) {
            client_destroy(client);
        }
        return;
    }

    /* Non-owner thread: dispatch recycle task to owner (callback checks state safely) */
    if (wp) {
        csilk_recycle_task_payload_t* p = malloc(sizeof(csilk_recycle_task_payload_t));
        if (!p) {
            return;
        }
        p->client = client;
        p->generation = client->generation;
        csilk_dispatch_task_t* task = _csilk_dispatch_task_alloc();
        if (!task) {
            free(p);
            return;
        }
        task->cb = _csilk_client_recycle_dispatch_cb;
        task->arg = p;
        task->client = NULL;
        csilk_lfq_enqueue(&wp->dispatch_queue, &task->lfq_node);
        csilk_io_async_send(&wp->dispatch_async);
    }
}

/**
 * @brief Teardown client context, arena, and return to worker-local pool.
 * @param client The client connection to destroy.
 */
void
client_destroy(csilk_client_t* client)
{
    if (!client) {
        return;
    }

    /* Atomic CAS: strictly ensure single, idempotent teardown */
    csilk_conn_state_t expected = CSILK_CONN_CLOSING;
    if (!atomic_compare_exchange_strong_explicit((_Atomic int*)&client->state,
                                                 (int*)&expected,
                                                 CSILK_CONN_CLOSED,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed)) {
        return;
    }

#ifdef CSILK_USE_URING
    if (client->handle.fd >= 0) {
        close(client->handle.fd);
        client->handle.fd = -1;
    }
#endif
    if (client->server) {
        atomic_fetch_sub_explicit(&client->server->active_connections, 1, memory_order_relaxed);
    }
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
    if (client->owner_pool && client->owner_pool->loop_ptr) {
        return client->owner_pool->loop_ptr;
    }
    return client->handle.loop ? client->handle.loop : csilk_io_default_loop();
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

static void
_csilk_on_close_owner(void* arg)
{
    csilk_close_task_payload_t* payload = (csilk_close_task_payload_t*)arg;
    if (!payload) {
        return;
    }
    csilk_io_handle_t* handle = payload->handle;
    uint64_t           generation = payload->generation;
    free(payload);
    if (!handle || !handle->data) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client->generation != generation || client->state == CSILK_CONN_CLOSED) {
        return;
    }
    on_close(handle);
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

    if (client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        return;
    }

    if (!_csilk_is_owner_worker_thread(client->owner_pool)) {
        csilk_close_task_payload_t* payload = malloc(sizeof(*payload));
        if (!payload) {
            return;
        }
        payload->handle = handle;
        payload->generation = client->generation;
        if (_csilk_dispatch_try(&client->ctx, _csilk_on_close_owner, payload) < 0) {
            free(payload);
        }
        return;
    }

    CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
    csilk_conn_set_state(client, CSILK_CONN_CLOSING);
    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
    client_list_remove(client->server, client);
    client->ctx.conn_closed = 1;

    _csilk_client_stop_timers(client);

    /* Release base connection reference held since on_new_connection */
    csilk_client_unref(client);
}
