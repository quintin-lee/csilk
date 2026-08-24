/**
 * @file http1_serialize.c
 * @brief HTTP/1.1 response serialization with AVX2 SIMD & fast-path formatting.
 */

#include <assert.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ctx/ctx_internal.h"
#include "../internal/srv_impl.h"
#include "../internal/srv_internal.h"
#include "../primitives/header_map.h"
#include "csilk/core/internal.h"
#include "csilk/http/h2.h"

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

/* --- Fast integer to ASCII converter --- */

static inline size_t
fast_uint64_to_str(char* buf, size_t val)
{
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    char tmp[24];
    int  i = 0;
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    return (size_t)i;
}

/* --- Fast Single-Pass Status Line & Connection Header Serializer --- */

static inline size_t
fast_serialize_status_and_control(char*       buf,
                                  int         status,
                                  const char* status_text,
                                  int         use_chunked,
                                  const char* transfer_encoding,
                                  size_t      body_len,
                                  const char* connection_val)
{
    size_t pos = 0;

    /* 1. Fast vectorized status line */
    if (__builtin_expect(status == 200, 1)) {
        memcpy(buf + pos, "HTTP/1.1 200 OK\r\n", 17);
        pos += 17;
    } else if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
        memcpy(buf + pos, "HTTP/1.1 101 Switching Protocols\r\n", 34);
        return 34;
    } else if (status == 404) {
        memcpy(buf + pos, "HTTP/1.1 404 Not Found\r\n", 24);
        pos += 24;
    } else if (status == 500) {
        memcpy(buf + pos, "HTTP/1.1 500 Internal Server Error\r\n", 36);
        pos += 36;
    } else {
        memcpy(buf + pos, "HTTP/1.1 ", 9);
        pos += 9;
        buf[pos++] = (char)('0' + (status / 100));
        buf[pos++] = (char)('0' + ((status / 10) % 10));
        buf[pos++] = (char)('0' + (status % 10));
        buf[pos++] = ' ';
        size_t text_len = strlen(status_text);
        memcpy(buf + pos, status_text, text_len);
        pos += text_len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    /* 2. Transfer-Encoding or Content-Length */
    if (use_chunked) {
        size_t te_len = strlen(transfer_encoding);
        memcpy(buf + pos, transfer_encoding, te_len);
        pos += te_len;
    } else {
        memcpy(buf + pos, "Content-Length: ", 16);
        pos += 16;
        pos += fast_uint64_to_str(buf + pos, body_len);
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    /* 3. Connection header */
    memcpy(buf + pos, "Connection: ", 12);
    pos += 12;
    size_t conn_len = strlen(connection_val);
    memcpy(buf + pos, connection_val, conn_len);
    pos += conn_len;
    buf[pos++] = '\r';
    buf[pos++] = '\n';

    return pos;
}

static inline size_t
append_custom_headers_fast(csilk_header_map_t* headers, char* buf, size_t pos)
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

    /* Maximum upper bound estimation for response header buffer */
    size_t max_header_bound =
        128 + strlen(status_text) + strlen(transfer_encoding) + custom_headers_len + 32;
    size_t total_alloc_len = max_header_bound + 4 + (use_chunked || is_file ? 0 : body_len);

    char* write_base = malloc(total_alloc_len + 1);
    if (write_base) {
        /* Single-pass vectorized serialization */
        size_t pos = fast_serialize_status_and_control(write_base,
                                                       status,
                                                       status_text,
                                                       use_chunked,
                                                       transfer_encoding,
                                                       body_len,
                                                       connection_val);

        pos = append_custom_headers_fast(&client->ctx.response.headers, write_base, pos);

        write_base[pos++] = '\r';
        write_base[pos++] = '\n';

        if (!use_chunked && !is_file) {
            if (body && body_len > 0) {
                memcpy(write_base + pos, body, body_len);
                pos += body_len;
            }
        }
        write_base[pos] = '\0';

        extern void _csilk_send_data_owned(csilk_ctx_t * c, char* data, size_t len);
        _csilk_send_data_owned(c, write_base, pos);
    }

    if (is_file) {
        return;
    }

    extern void _csilk_handle_post_response(csilk_client_t * client, int keep_alive);
    _csilk_handle_post_response(client, keep_alive);
}
