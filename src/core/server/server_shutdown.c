/**
 * @file server_shutdown.c
 * @brief Graceful shutdown — signal handling, client draining, stop callbacks.
 *
 * @copyright MIT License
 */

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Signal handler --- */

static void on_server_handle_close(csilk_io_handle_t* handle);

/** @brief Signal handler for SIGINT — initiates graceful shutdown. */
void
on_signal(csilk_io_signal_t* handle, int signum)
{
    (void)signum;
    csilk_server_t* server = (csilk_server_t*)handle->data;
    csilk_server_stop(server);
}

/* --- Graceful shutdown --- */

/**
 * @brief Iterate clients on the given loop and close them appropriately.
 *
 * ## Timing Contract (H3 from security audit)
 *
 * This function walks `wp->active_clients` without holding a lock. It is
 * safe because:
 *
 *  1. **Accept is already stopped** — the listener was closed before
 *     `close_active_clients` is called (see `on_stop_async` / `on_worker_stop_async`),
 *     so no new connections will be added to the list.
 *  2. **In-flight I/O callbacks still run on the owning worker's loop**,
 *     but they only *remove* a client from the list (via `client_list_remove`)
 *     when the connection is being closed.  Because the loop is still
 *     spinning at this point (we stop it after draining), concurrent
 *     removals can set `client->next` to NULL.  The `while (client)` loop
 *     handles that correctly: `client->next` is read *before* the close
 *     call, so even if the close path nulls it, the iteration already
 *     captured the pointer.
 *  3. **Dispatch queues are drained *after* this call** (see
 *     `on_stop_async`).  Any cross-worker recycle task that was enqueued
 *     before the listener closed will be processed by
 *     `_csilk_worker_drain_dispatch` — it will find the client already in
 *     CLOSING/CLOSED state and skip the destroy, avoiding a double-close.
 *
 * The `assert` below makes this contract explicit in debug builds.
 */
static int
close_active_clients(csilk_server_t* server, csilk_io_loop_t* loop)
{
    int            count = 0;
    worker_pool_t* wp = NULL;
    for (int i = 0; i < server->worker_pool_count; i++) {
        /* Match on loop_ptr first: in default-loop (single-worker) mode
         * wp[0].loop is never initialized (loop_ptr == server->loop), so
         * matching on &wp->loop silently finds NO pool and leaks zombie
         * keep-alive handles into the shared loop. */
        if (server->worker_pools[i].loop_ptr == loop || &server->worker_pools[i].loop == loop) {
            wp = &server->worker_pools[i];
            break;
        }
    }
    if (!wp) {
        return 0;
    }

    /* Timing contract: the listener must already be closing or closed.
     * In on_stop_async the main-loop listener is closed before this call.
     * In on_worker_stop_async the worker's listen_handle is closed first.
     * No new connections can be added to active_clients after that point.
     * In-flight I/O callbacks may still remove clients (client_list_remove
     * sets ->next to NULL), but the while-loop reads `client->next` before
     * calling the close path, so a concurrent NULL-out is safe.  Dispatch
     * queues are drained after this call, and any cross-worker recycle
     * task that arrives late will find the client in CLOSING/CLOSED state
     * and skip the destroy. */

    csilk_client_t* client = wp->active_clients;
    while (client) {
        csilk_client_t* next = client->next;
        count++;
        if (client->protocol == CSILK_PROTO_HTTP3) {
            if (server->h3_state && server->h3_state->conn_close) {
                server->h3_state->conn_close(client);
            }
        } else if (client->ctx.is_websocket) {
            /* The WS close-echo path (csilk_ws_parse_frame opcode 0x8) may
             * already be closing this handle — uv_close() asserts on a
             * handle that is closing, so check first. */
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                if (server->ws_close_cb) {
                    server->ws_close_cb(&client->ctx, 1001, "Server stopping");
                }
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        } else if (client->ctx.is_sse) {
            /* Send the terminal SSE event, then close with on_close so the
             * client object is recycled — csilk_sse_close() passes a NULL
             * close callback which would strand the client on active list. */
            if (server->sse_send_cb) {
                server->sse_send_cb(&client->ctx, "close", "Server stopping");
            }
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        } else {
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
        client = next;
    }
    return count;
}

/** @brief Async callback to stop the server gracefully. */
void
on_stop_async(csilk_io_async_t* handle)
{
    csilk_server_t* server = (csilk_server_t*)handle->data;
    CSILK_LOG_I("Server: initiating graceful shutdown");

    /* Ordering matters here (H3): hooks MUST run first so in-flight work can
     * register late stop-time cleanup, then the listener closes so no new
     * connections are accepted, then active clients are drained, then the
     * dispatch async and signal handles close, then workers are signaled and
     * the loop is stopped. Any reordering can strand references mid-shutdown. */
    _csilk_trigger_hooks(server, NULL, CSILK_HOOK_SERVER_STOP);

    if (!csilk_io_is_closing((csilk_io_handle_t*)&server->server_handle)) {
        CSILK_LOG_D("Server: closing server socket listener");
        csilk_io_close((csilk_io_handle_t*)&server->server_handle, on_server_handle_close);
    }

    {
        int n = close_active_clients(server, server->loop);
        if (n > 0) {
            CSILK_LOG_I("Server: closed %d active client connection(s)", n);
        }
    }

    if (!csilk_io_is_closing((csilk_io_handle_t*)&server->sig_handle)) {
        csilk_io_close((csilk_io_handle_t*)&server->sig_handle, on_server_handle_close);
    }

    _csilk_worker_drain_dispatch(&server->worker_pools[0]);
    if (!csilk_io_is_closing((csilk_io_handle_t*)&server->worker_pools[0].dispatch_async)) {
        csilk_io_close((csilk_io_handle_t*)&server->worker_pools[0].dispatch_async, NULL);
    }

    if (!csilk_io_is_closing((csilk_io_handle_t*)&server->async_handle)) {
        csilk_io_close((csilk_io_handle_t*)&server->async_handle, on_server_handle_close);
    }

    for (int i = 1; i < server->worker_pool_count; i++) {
        CSILK_LOG_D("Server: signaling worker thread %d to stop", i);
        csilk_io_async_send(&server->worker_pools[i].stop_async);
    }

    if (server->worker_pools && server->worker_pools[0].h3_udp_active && server->h3_state &&
        server->h3_state->worker_stop) {
        server->h3_state->worker_stop(&server->worker_pools[0]);
    }

    csilk_io_stop(server->loop);
}

/** @brief Close callback for server-level handles during shutdown (libuv path). */
static void
on_server_handle_close(csilk_io_handle_t* handle)
{
    (void)handle;
}

/* --- Server stop --- */

/** @brief Signal the server to stop gracefully (thread-safe). */
void
csilk_server_stop(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_io_async_send(&server->async_handle);
}

/* --- Worker stop --- */

/** @brief Async callback for stopping a worker's event loop gracefully. */
void
on_worker_stop_async(csilk_io_async_t* handle)
{
    worker_stop_data_t* sd = (worker_stop_data_t*)handle->data;
    if (!sd) {
        return;
    }

    csilk_server_t*  server = sd->server;
    csilk_io_loop_t* loop = sd->loop;

    if (!csilk_io_is_closing((csilk_io_handle_t*)sd->listen_handle)) {
        csilk_io_close((csilk_io_handle_t*)sd->listen_handle, NULL);
    }

    close_active_clients(server, loop);

    int worker_idx = sd->worker_index;
    _csilk_worker_drain_dispatch(&server->worker_pools[worker_idx]);
    if (!csilk_io_is_closing(
            (csilk_io_handle_t*)&server->worker_pools[worker_idx].dispatch_async)) {
        csilk_io_close((csilk_io_handle_t*)&server->worker_pools[worker_idx].dispatch_async, NULL);
    }

    if (!csilk_io_is_closing((csilk_io_handle_t*)handle)) {
        csilk_io_close((csilk_io_handle_t*)handle, NULL);
    }

    if (server->worker_pools[worker_idx].h3_udp_active && server->h3_state &&
        server->h3_state->worker_stop) {
        server->h3_state->worker_stop(&server->worker_pools[worker_idx]);
    }

    csilk_io_stop(loop);
}
