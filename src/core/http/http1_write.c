/**
 * @file http1_write.c
 * @brief HTTP/1.1 write pipeline and sendfile handling.
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

/* --- Sendfile completion --- */

static void
on_sendfile_complete(csilk_io_fs_t* req)
{
    csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_io_fs_req_cleanup(req);
    free(req);

    if (!client) {
        return;
    }

    int keep_alive = llhttp_should_keep_alive(&client->parser);
    client->keep_alive = (int)keep_alive;

    if (client->server->config.write_timeout_ms > 0) {
        csilk_io_timer_stop(&client->write_timer);
    }

    extern void on_idle_timeout(csilk_io_timer_t * handle);
    extern void on_close(csilk_io_handle_t * handle);
    extern void csilk_client_read_start(csilk_client_t * client);
    extern void _csilk_trigger_hooks(csilk_server_t * s, csilk_ctx_t * c, csilk_hook_type_t type);
    extern void csilk_ctx_cleanup(csilk_ctx_t * c);

    if (keep_alive) {
        csilk_io_timer_start(
            &client->timer, on_idle_timeout, client->server->config.idle_timeout_ms, 0);
        csilk_client_read_start(client);
    } else {
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }

    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);
    csilk_ctx_cleanup(&client->ctx);
}

/* --- Write completion --- */

void
on_write(csilk_io_write_t* req, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Write error: %s", csilk_io_strerror(status));
    }
    csilk_client_t* client = NULL;
    if (req->handle) {
        client = (csilk_client_t*)req->handle->data;
        if (client) {
            csilk_io_timer_stop(&client->write_timer);
        }
    }

    if (req->data) {
        free(req->data);
    }

    if (client && client->ctx.file_fd >= 0) {
        csilk_io_os_fd_t sock_fd;
        if (csilk_io_fileno((const csilk_io_handle_t*)&client->handle, &sock_fd) == 0) {
            csilk_io_fs_t* fs_req = malloc(sizeof(csilk_io_fs_t));
            if (fs_req) {
                fs_req->data = &client->ctx;
                int    fd = client->ctx.file_fd;
                size_t offset = client->ctx.file_offset;
                size_t size = client->ctx.file_size;
                client->ctx.file_fd = -1;

                int r = csilk_io_fs_sendfile(csilk_io_default_loop(),
                                             fs_req,
                                             sock_fd,
                                             fd,
                                             offset,
                                             size,
                                             on_sendfile_complete);
                if (r < 0) {
                    free(fs_req);
                } else {
                    free(req);
                    return;
                }
            }
        }
    }

    free(req);

    if (client) {
        extern void _csilk_check_and_trigger_drain(csilk_client_t * client);
        _csilk_check_and_trigger_drain(client);
    }
}

/* --- Client write --- */

void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t len)
{
    if (!client || client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        return;
    }

    assert(len <= INT_MAX);

    if (client->ssl) {
        extern void flush_tls_write(csilk_client_t * client);
        SSL_write(client->ssl, data, (int)len);
        flush_tls_write(client);
        return;
    }

    csilk_io_write_t* req = malloc(sizeof(csilk_io_write_t));
    if (!req) {
        return;
    }

    char* buf_copy = malloc(len);
    if (!buf_copy) {
        free(req);
        return;
    }
    memcpy(buf_copy, data, len);

    csilk_io_buf_t buf = csilk_io_buf_init(buf_copy, (unsigned int)len);
    req->data = buf_copy;
    csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_write);
}

/* --- Send data helpers --- */

CSILK_INTERNAL size_t
_csilk_client_get_write_queue_size(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
#ifndef CSILK_USE_URING
    return ((csilk_io_stream_t*)&client->handle)->write_queue_size;
#else
    return client->pending_write_bytes;
#endif
}

CSILK_INTERNAL void
_csilk_check_and_trigger_drain(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    csilk_ctx_t* c = &client->ctx;
    if (c->write_paused) {
        size_t q = _csilk_client_get_write_queue_size(client);
        if (q <= c->write_low_water_mark) {
            c->write_paused = 0;
            if (c->on_drain) {
                void (*drain_cb)(csilk_ctx_t*, void*) = c->on_drain;
                void* drain_data = c->on_drain_data;
                drain_cb(c, drain_data);
            }
        }
    }
}

CSILK_INTERNAL void
_csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        return;
    }
    csilk_client_write(client, data, len);
}

CSILK_INTERNAL void
_csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len)
{
    if (!data) {
        return;
    }
    if (!c || c->conn_closed || !c->_internal_client) {
        free(data);
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        free(data);
        return;
    }

    if (client->ssl) {
        assert(len <= INT_MAX);
        extern void flush_tls_write(csilk_client_t * client);
        SSL_write(client->ssl, (const uint8_t*)data, (int)len);
        flush_tls_write(client);
        free(data);
        return;
    }

    csilk_io_write_t* req = malloc(sizeof(csilk_io_write_t));
    if (!req) {
        free(data);
        return;
    }

    req->data = data;
    csilk_io_buf_t buf = csilk_io_buf_init(data, (unsigned int)len);
    csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_write);
}
