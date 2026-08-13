/**
 * @file http1_response.c
 * @brief HTTP/1.1 response building: status line, headers, serialization,
 *        write pipeline, and keep-alive logic.
 *
 * @copyright MIT License
 */

#include <assert.h>
#include <limits.h>
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

/** @brief sendfile completion callback — handles keep-alive and
 *  cleanup after a zero-copy file send.
 *
 *  Called by the I/O backend when csilk_io_fsSendfile completes.
 *  Frees the filesystem request, then checks the connection state:
 *   - If keep-alive is negotiated, restarts the idle timer and resumes
 *     reading (uv_read_start) for the next request.
 *   - Otherwise, closes the TCP handle (csilk_io_close).
 *  In both cases, fires CSILK_HOOK_REQUEST_END and cleans up the context.
 *
 *  @param req The completed csilk_io_fs_t request. req->data points to csilk_ctx_t.
 *             The request and its associated buffer are freed by this callback. */
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

/** @brief Write completion callback — handles post-write pipeline.
 *
 * After a response body (or TLS-encrypted data) has been written to the
 * socket, this callback orchestrates the next action:
 *
 *   1. If the response includes a file descriptor (file_fd >= 0), the
 *      sendfile pipeline is triggered: csilk_io_fs_sendfile() is called to
 *      stream file data directly from the kernel page cache to the socket.
 *      This path is only used for non-TLS connections.
 *
 *   2. If no file descriptor is pending, the write request is freed and
 *      the connection's keep-alive/close decision is handled by the
 *      caller (_csilk_send_response, which already set up timers).
 *
 *   3. On write error, logs the failure and does NOT retry (the caller
 *      is expected to close the connection via the read callback or timer).
 *
 * The write request's data buffer (buf_copy) is freed here because it
 * was allocated by _csilk_send_data / flush_tls_write.
 *
 * @param req    The completed csilk_io_write_t request.
 * @param status 0 on success, negative on error. */
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
}

/* --- Status text --- */

/** @brief Map an HTTP status code to its standard reason phrase.
 *
 * Supports common codes: 101, 200, 201, 204, 400, 401, 403, 404, 500.
 * Unrecognized codes default to "OK".
 *
 * @param status HTTP status code.
 * @return A static string literal with the reason phrase. */
const char*
get_status_text(int status)
{
    switch (status) {
    case CSILK_STATUS_SWITCHING_PROTOCOLS:
        return "Switching Protocols";
    case CSILK_STATUS_OK:
        return "OK";
    case CSILK_STATUS_CREATED:
        return "Created";
    case CSILK_STATUS_NO_CONTENT:
        return "No Content";
    case CSILK_STATUS_BAD_REQUEST:
        return "Bad Request";
    case CSILK_STATUS_UNAUTHORIZED:
        return "Unauthorized";
    case CSILK_STATUS_FORBIDDEN:
        return "Forbidden";
    case CSILK_STATUS_NOT_FOUND:
        return "Not Found";
    case CSILK_STATUS_INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

/* --- Client write --- */

/** @brief Send raw data to the client (TLS-aware).
 *
 * If TLS is active, writes through the SSL session and flushes the write BIO.
 * Otherwise, allocates a write request, copies the data, and queues the write
 * via the I/O backend. The data buffer is freed by the write completion callback.
 *
 * @param client The client connection.
 * @param data   Data buffer to send.
 * @param len    Length of data in bytes. */
#ifndef CSILK_USE_URING
void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t len)
{
    if (!client) {
        return;
    }

    /* Guard against size_t-to-int truncation for SSL_write and csilk_io_buf_init. */
    assert(len <= INT_MAX);

    if (client->ssl) {
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
#endif

/* --- Send data --- */

/** @brief Write raw data to the client connection.
 *
 * Extracts the internal client from the context and delegates the write
 * to the client's buffered write path. Used internally by the response
 * sender to flush serialised HTTP data.
 *
 * @param c    The request context.
 * @param data Pointer to the data buffer.
 * @param len  Number of bytes to write. */
CSILK_INTERNAL void
_csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_client_write(client, data, len);
}

/** @brief Send data with ownership transfer — caller's buffer is freed by
 *  the write callback (or immediately for TLS).
 *
 *  Unlike _csilk_send_data() / csilk_client_write(), this does NOT make
 *  an internal copy. Instead the caller's heap buffer is passed directly
 *  to csilk_io_write() and freed by on_write().  For TLS connections the buffer
 *  is freed immediately after SSL_write(). */
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

    if (client->ssl) {
        assert(len <= INT_MAX);
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

/* --- Response serialization --- */

/**
 * @brief Serialize the HTTP/1.1 status line and headers into a buffer.
 *
 * Handles three modes: 101 Switching Protocols (minimal headers), chunked
 * transfer encoding, and normal Content-Length mode.  Call with NULL/0 for
 * @p buf/@p buf_size to compute the required length without writing.
 *
 * @param buf         Output buffer (NULL to compute length only).
 * @param buf_size    Size of output buffer (ignored when buf is NULL).
 * @param status      HTTP status code.
 * @param status_text Corresponding reason phrase (e.g. "OK").
 * @param use_chunked Non-zero to emit Transfer-Encoding: chunked.
 * @param transfer_encoding "Transfer-Encoding: chunked\r\n" or "".
 * @param body_len    Response body length (for Content-Length header).
 * @param connection_val "keep-alive" or "close".
 * @return Number of bytes that would be written (excluding NUL), or -1 on error.
 */
static int
serialize_status_line(char*       buf,
                      size_t      buf_size,
                      int         status,
                      const char* status_text,
                      int         use_chunked,
                      const char* transfer_encoding,
                      size_t      body_len,
                      const char* connection_val)
{
    if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
        return snprintf(buf, buf_size, "HTTP/1.1 101 Switching Protocols\r\n");
    } else if (use_chunked) {
        return snprintf(buf,
                        buf_size,
                        "HTTP/1.1 %d %s\r\n"
                        "%s"
                        "Connection: %s\r\n",
                        status,
                        status_text,
                        transfer_encoding,
                        connection_val);
    } else {
        return snprintf(buf,
                        buf_size,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n",
                        status,
                        status_text,
                        body_len,
                        connection_val);
    }
}

/**
 * @brief Append all custom response headers to a buffer.
 *
 * Iterates the header hash map and writes each key-value pair in
 * "Key: Value\r\n" format starting at @p pos in @p buf.
 *
 * @param headers The response header map.
 * @param buf     Output buffer.
 * @param pos     Starting position within @p buf.
 * @return New position after all headers have been written.
 */
static size_t
append_custom_headers(csilk_header_map_t* headers, char* buf, size_t pos)
{
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = headers->buckets[i]; h; h = h->next) {
            memcpy(buf + pos, h->key, h->key_len);
            pos += h->key_len;
            buf[pos++] = ':';
            buf[pos++] = ' ';
            memcpy(buf + pos, h->value, h->value_len);
            pos += h->value_len;
            buf[pos++] = '\r';
            buf[pos++] = '\n';
        }
    }
    return pos;
}

/**
 * @brief Post-response cleanup: timers, keep-alive, and WebSocket state.
 *
 * Stops the read timer, starts the write-timeout guard and either the
 * idle timer (keep-alive) or closes the connection.  Preserves WebSocket
 * callback state across csilk_ctx_cleanup().
 *
 * @param client     The client connection.
 * @param keep_alive Non-zero to keep the connection alive.
 */
CSILK_INTERNAL void
_csilk_handle_post_response(csilk_client_t* client, int keep_alive)
{
    csilk_io_timer_stop(&client->read_timer);

    if (client->server->config.write_timeout_ms > 0) {
        csilk_io_timer_start(
            &client->write_timer, on_write_timeout, client->server->config.write_timeout_ms, 0);
    }

    int   is_ws = client->ctx.is_websocket;
    void* ws_msg_cb = client->ctx.on_ws_message;
    void* ws_send_cb = client->ctx.on_ws_send;

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
        csilk_io_timer_start(
            &client->timer, on_idle_timeout, client->server->config.idle_timeout_ms, 0);
        llhttp_resume(&client->parser);
        csilk_client_read_start(client);
    } else {
        CSILK_LOG_I("_csilk_handle_post_response: closing handle");
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }
}

CSILK_INTERNAL void
_csilk_send_response(csilk_ctx_t* c)
{
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client) {
        return;
    }

    if (client->protocol == CSILK_PROTO_HTTP2) {
        csilk_h2_send_response(c);
        return;
    }

    csilk_io_timer_stop(&client->request_timer);

    int         status = client->ctx.response.status ? client->ctx.response.status : 200;
    const char* status_text = get_status_text(status);

    int is_file = (c->file_fd >= 0 && !client->ssl);
    int use_chunked = (client->ctx.response.body_len == 0 && client->ctx.is_async && !is_file);
    const char* transfer_encoding = use_chunked ? "Transfer-Encoding: chunked\r\n" : "";

    size_t custom_headers_len = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h; h = h->next) {
            custom_headers_len += h->key_len + 2 + h->value_len + 2;
        }
    }

    size_t      body_len = is_file ? c->file_size : client->ctx.response.body_len;
    const char* body = client->ctx.response.body ? client->ctx.response.body : "";

    int keep_alive = llhttp_should_keep_alive(&client->parser);
    client->keep_alive = (int)keep_alive;
    const char* connection_val = keep_alive ? "keep-alive" : "close";

    /* Serialise status line (NULL/0 computes required length) */
    int header_len = serialize_status_line(
        NULL, 0, status, status_text, use_chunked, transfer_encoding, body_len, connection_val);
    if (header_len < 0) {
        return;
    }

    size_t response_len =
        (size_t)header_len + custom_headers_len + 2 + (use_chunked || is_file ? 0 : body_len);

    char* write_base = malloc(response_len + 1);
    if (write_base) {
        int snp = serialize_status_line(write_base,
                                        response_len + 1,
                                        status,
                                        status_text,
                                        use_chunked,
                                        transfer_encoding,
                                        body_len,
                                        connection_val);
        if (snp < 0) {
            free(write_base);
            return;
        }
        size_t pos = (size_t)snp;

        pos = append_custom_headers(&client->ctx.response.headers, write_base, pos);

        if (!use_chunked && !is_file) {
            size_t remain = response_len + 1 - pos;
            snprintf(write_base + pos, remain, "\r\n%s", body);
        } else {
            size_t remain = response_len + 1 - pos;
            snprintf(write_base + pos, remain, "\r\n");
        }

        _csilk_send_data(c,
                         (const uint8_t*)write_base,
                         (use_chunked || is_file ? (size_t)pos + 2 : response_len));
        free(write_base);
    }

    if (is_file) {
        return;
    }

    _csilk_handle_post_response(client, keep_alive);
}
