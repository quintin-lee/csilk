/**
 * @file http1_pipeline.c
 * @brief HTTP/1.1 post-response cleanup and pipeline logic.
 */

#include <assert.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../primitives/header_map.h"
#include "h2.h"
#include "../internal/srv_impl.h"

/* --- Post-response cleanup --- */

CSILK_INTERNAL void
_csilk_handle_post_response(csilk_client_t* client, int keep_alive)
{
    csilk_io_timer_stop(&client->read_timer);

    unsigned int write_timeout = _csilk_server_get_write_timeout_ms(client->server);
    if (write_timeout > 0) {
        extern void on_write_timeout(csilk_io_timer_t * handle);
        csilk_io_timer_start(&client->write_timer, on_write_timeout, write_timeout, 0);
    }

    int   is_ws = client->ctx.is_websocket;
    void* ws_msg_cb = client->ctx.on_ws_message;
    void* ws_send_cb = client->ctx.on_ws_send;

    extern void _csilk_trigger_hooks(csilk_server_t * s, csilk_ctx_t * c, csilk_hook_type_t type);
    extern void csilk_ctx_cleanup(csilk_ctx_t * c);
    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);

    csilk_ctx_cleanup(&client->ctx);

    if (is_ws) {
        client->ctx.is_websocket = is_ws;
        client->ctx.on_ws_message = ws_msg_cb;
        client->ctx.on_ws_send = ws_send_cb;
    }

    if (client->ctx.is_websocket) {
        return;
    }

    CSILK_LOG_I("_csilk_handle_post_response called, keep_alive=%d", keep_alive);
    if (keep_alive) {
        CSILK_LOG_I("_csilk_handle_post_response: restarting read");
        extern void csilk_conn_set_state(csilk_client_t * client, csilk_conn_state_t new_state);
        extern void on_idle_timeout(csilk_io_timer_t * handle);
        extern void csilk_client_read_start(csilk_client_t * client);
        csilk_conn_set_state(client, CSILK_CONN_READING);
        unsigned int idle_timeout = _csilk_server_get_idle_timeout_ms(client->server);
        csilk_io_timer_start(&client->timer, on_idle_timeout, idle_timeout, 0);
        llhttp_resume(&client->parser);
        csilk_client_read_start(client);
    } else {
        CSILK_LOG_I("_csilk_handle_post_response: closing handle");
        extern void csilk_conn_set_state(csilk_client_t * client, csilk_conn_state_t new_state);
        extern void on_close(csilk_io_handle_t * handle);
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }
}
