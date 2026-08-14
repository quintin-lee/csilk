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
#include "messaging/mq_internal.h"

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
 */
static int
close_active_clients(csilk_server_t* server, csilk_io_loop_t* loop)
{
    int            count = 0;
    worker_pool_t* wp = NULL;
    for (int i = 0; i < server->worker_pool_count; i++) {
        if (&server->worker_pools[i].loop == loop) {
            wp = &server->worker_pools[i];
            break;
        }
    }
    if (!wp) {
        return 0;
    }

    csilk_client_t* client = wp->active_clients;
    while (client) {
        count++;
        if (client->ctx.is_websocket) {
            csilk_ws_close(&client->ctx, 1001, "Server stopping");
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        } else if (client->ctx.is_sse) {
            csilk_sse_send(&client->ctx, "close", "Server stopping");
            csilk_sse_close(&client->ctx);
        } else {
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
        client = client->next;
    }
    return count;
}

/** @brief Async callback to stop the server gracefully. */
void
on_stop_async(csilk_io_async_t* handle)
{
    csilk_server_t* server = (csilk_server_t*)handle->data;
    CSILK_LOG_I("Server: initiating graceful shutdown");

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

    if (server->mq) {
        CSILK_LOG_D("Server: freeing message queue");
        _csilk_mq_free(server->mq);
        server->mq = NULL;
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
    if (!csilk_io_is_closing(
            (csilk_io_handle_t*)&server->worker_pools[worker_idx].dispatch_async)) {
        csilk_io_close((csilk_io_handle_t*)&server->worker_pools[worker_idx].dispatch_async, NULL);
    }

    if (!csilk_io_is_closing((csilk_io_handle_t*)handle)) {
        csilk_io_close((csilk_io_handle_t*)handle, NULL);
    }

    csilk_io_stop(loop);
}
