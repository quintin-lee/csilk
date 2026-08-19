/**
 * @file http1_serialize.c
 * @brief HTTP/1.1 response serialization.
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

/* --- Status text --- */

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

/* --- Serialization helpers --- */

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

/* --- Main response sender --- */

CSILK_INTERNAL void
_csilk_send_response(csilk_ctx_t* c)
{
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client) {
        return;
    }

    csilk_conn_set_state(client, CSILK_CONN_WRITING);

    if (client->protocol == CSILK_PROTO_HTTP2) {
        extern void csilk_h2_send_response(csilk_ctx_t * c);
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
            write_base[pos++] = '\r';
            write_base[pos++] = '\n';
            if (body && body_len > 0) {
                memcpy(write_base + pos, body, body_len);
                pos += body_len;
            }
            write_base[pos] = '\0';
        } else {
            write_base[pos++] = '\r';
            write_base[pos++] = '\n';
            write_base[pos] = '\0';
        }

        extern void _csilk_send_data_owned(csilk_ctx_t * c, char* data, size_t len);
        _csilk_send_data_owned(
            c, write_base, (use_chunked || is_file ? (size_t)pos : response_len));
    }

    if (is_file) {
        return;
    }

    extern void _csilk_handle_post_response(csilk_client_t * client, int keep_alive);
    _csilk_handle_post_response(client, keep_alive);
}
