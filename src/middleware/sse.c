/**
 * @file sse.c
 * @brief Server-Sent Events (SSE) implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "csilk.h"
#include "csilk_internal.h"

/** @brief SSE write completion callback. @param req Write request. @param status Write status. */
static void on_sse_write(uv_write_t* req, int status) {
    if (status < 0) {
        CSILK_LOG_E("SSE write error: %s", uv_strerror(status));
    }
    if (req->data) free(req->data);
    free(req);
}

/** @brief Initialize an SSE connection with proper headers and status. */
void csilk_sse_init(csilk_ctx_t* c) {
    if (!c) return;

    csilk_set_header(c, "Content-Type", "text/event-stream");
    csilk_set_header(c, "Cache-Control", "no-cache");
    csilk_set_header(c, "Connection", "keep-alive");
    csilk_set_header(c, "X-Accel-Buffering", "no");

    c->response.status = 200;
    c->is_websocket = 1;

    if (!c->_internal_client) return;

    const char* hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: keep-alive\r\n"
                      "X-Accel-Buffering: no\r\n"
                      "\r\n";

    size_t hdr_len = strlen(hdr);
    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) return;

    char* buf = malloc(hdr_len);
    if (!buf) {
        free(req);
        return;
    }
    memcpy(buf, hdr, hdr_len);

    uv_buf_t uv_buf = uv_buf_init(buf, (unsigned int)hdr_len);
    req->data = buf;
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    uv_write(req, stream, &uv_buf, 1, on_sse_write);
}

/** @brief Send an SSE event (optional event type + data). */
void csilk_sse_send(csilk_ctx_t* c, const char* event, const char* data) {
    if (!c || !c->_internal_client) return;

    size_t event_len = event ? strlen(event) : 0;
    size_t data_len = data ? strlen(data) : 0;
    size_t buf_size = (event ? 7 + event_len + 1 : 0) + (data ? 6 + data_len + 1 : 0) + 1;
    char* buf = malloc(buf_size);
    if (!buf) return;

    int pos = 0;
    if (event && event_len > 0) {
        pos += snprintf(buf + pos, buf_size - pos, "event: %s\n", event);
    }
    if (data) {
        pos += snprintf(buf + pos, buf_size - pos, "data: %s\n", data);
    }
    pos += snprintf(buf + pos, buf_size - pos, "\n");

    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) {
        free(buf);
        return;
    }

    uv_buf_t uv_buf = uv_buf_init(buf, (unsigned int)pos);
    req->data = buf;
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    uv_write(req, stream, &uv_buf, 1, on_sse_write);
}

/** @brief Close the SSE connection. */
void csilk_sse_close(csilk_ctx_t* c) {
    if (!c || !c->_internal_client) return;
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    if (!uv_is_closing((uv_handle_t*)stream)) {
        uv_close((uv_handle_t*)stream, NULL);
    }
}
