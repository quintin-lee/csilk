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

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

/**
 * @brief SSE write completion callback.
 *
 * Called by libuv after an asynchronous write to the SSE client socket
 * completes (or fails). On failure, logs the error via CSILK_LOG_E.
 * In either case frees the write request and the associated buffer.
 *
 * @param req     The completed uv_write_t request. req->data points to the
 *                heap-allocated buffer that was written.
 * @param status  0 on success, negative on error (UV_E* codes).
 */
static void on_sse_write(uv_write_t* req, int status) {
  if (status < 0) {
    CSILK_LOG_E("SSE write error: %s", uv_strerror(status));
  }
  if (req->data) free(req->data);
  free(req);
}

/**
 * @brief Initialize an SSE connection with proper headers and status.
 *
 * Sets the Content-Type, Cache-Control, Connection, and X-Accel-Buffering
 * headers, marks the context as an SSE connection (is_sse = 1), and writes
 * the raw HTTP 200 OK response headers directly to the client socket.
 *
 * @param c  The request context.
 *
 * @note After calling csilk_sse_init(), the connection is established and
 *       the caller should not call csilk_next() — SSE hijacks the socket.
 * @warning The headers are written as a raw string bypassing the normal
 *          response pipeline. The connection remains open for the lifetime
 *          of the SSE stream.
 */
void csilk_sse_init(csilk_ctx_t* c) {
  if (!c) return;

  csilk_set_header(c, "Content-Type", "text/event-stream");
  csilk_set_header(c, "Cache-Control", "no-cache");
  csilk_set_header(c, "Connection", "keep-alive");
  csilk_set_header(c, "X-Accel-Buffering", "no");

  c->response.status = CSILK_STATUS_OK;
  c->is_sse = 1;

  if (!c->_internal_client) return;

  const char* hdr =
      "HTTP/1.1 200 OK\r\n"
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

/**
 * @brief Send an SSE event (optional event type + data).
 *
 * Formats and writes a Server-Sent Event message to the client. If an event
 * type is provided, an "event:" line is emitted. The data is written as a
 * "data:" line. Both are terminated by an empty line ("\n") as required by
 * the SSE specification (RFC 8895 §2).
 *
 * @param c     The request context (must be in SSE mode).
 * @param event Optional event type string (e.g. "message", "update").
 *              May be NULL, in which case no "event:" line is emitted.
 * @param data  The event payload string. May be NULL (produces a "data:" line
 *              with no content). Must not contain embedded "\n\n" sequences
 *              unless multi-line data is intended per SSE spec.
 *
 * @note Each send allocates a new buffer and write request. For high-
 *       frequency event streams, consider batching or a pooled allocator.
 * @warning This function does NOT check for backpressure. If the client
 *          reads slower than the server writes, the kernel buffer may fill
 *          and writes will fail with EAGAIN (reported via on_sse_write).
 */
void csilk_sse_send(csilk_ctx_t* c, const char* event, const char* data) {
  if (!c || !c->_internal_client) return;

  size_t event_len = event ? strlen(event) : 0;
  size_t data_len = data ? strlen(data) : 0;
  size_t buf_size =
      (event ? 7 + event_len + 1 : 0) + (data ? 6 + data_len + 1 : 0) + 1;
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

/**
 * @brief Close the SSE connection.
 *
 * Closes the underlying libuv stream handle for the SSE client connection.
 * Performs a graceful close via uv_close(). Any pending write requests will
 * complete before the handle is fully closed.
 *
 * @param c  The request context (must be in SSE mode with an active client).
 */
void csilk_sse_close(csilk_ctx_t* c) {
  if (!c || !c->_internal_client) return;
  uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
  if (!uv_is_closing((uv_handle_t*)stream)) {
    uv_close((uv_handle_t*)stream, NULL);
  }
}
