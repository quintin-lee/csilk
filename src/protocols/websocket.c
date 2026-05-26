/**
 * @file websocket.c
 * @brief WebSocket handshake, frame encoding, and frame parsing implementation.
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

/** @brief WebSocket magic GUID string per RFC 6455 Section 4.2.2.
 *
 * Appended to the client's Sec-WebSocket-Key before SHA-1 hashing to produce
 * the Sec-WebSocket-Accept response header. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/** @brief Perform the WebSocket upgrade handshake per RFC 6455.
 *
 * Validates the presence of the Sec-WebSocket-Key header, computes the
 * expected Sec-WebSocket-Accept response by concatenating the key with the
 * WebSocket GUID ("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"), SHA-1 hashing
 * the result, and Base64-encoding the digest. Sets the Upgrade, Connection,
 * and Sec-WebSocket-Accept response headers, sets status to 101 Switching
 * Protocols, and marks the context as a WebSocket connection.
 *
 * @param c The request context.
 * @note After a successful handshake, the context's is_websocket flag is set
 *       and the connection is ready for frame I/O via csilk_ws_send() and
 *       csilk_ws_parse_frame(). On failure (missing Sec-WebSocket-Key), a
 *       400 Bad Request JSON error is sent. */
void csilk_ws_handshake(csilk_ctx_t* c) {
  const char* key = csilk_get_header(c, "Sec-WebSocket-Key");
  if (!key) {
    csilk_json_error(c, CSILK_STATUS_BAD_REQUEST,
                     "Upgrade Required: Sec-WebSocket-Key missing");
    return;
  }

  char combined[256];
  snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

  csilk_sha1_ctx sha_ctx;
  uint8_t digest[20];
  csilk_sha1_init(&sha_ctx);
  csilk_sha1_update(&sha_ctx, (uint8_t*)combined, (uint32_t)strlen(combined));
  csilk_sha1_final(&sha_ctx, digest);

  char accept_key[32];
  csilk_base64_encode(digest, 20, accept_key);

  csilk_set_header(c, "Upgrade", "websocket");
  csilk_set_header(c, "Connection", "Upgrade");
  csilk_set_header(c, "Sec-WebSocket-Accept", accept_key);

  csilk_status(c, CSILK_STATUS_SWITCHING_PROTOCOLS);
  c->is_websocket = 1;
}

/** @brief libuv write completion callback for WebSocket frame sends.
 *
 * Frees the frame buffer and the write request structure. Errors are
 * silently ignored.
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (ignored). */
static void on_ws_write(uv_write_t* req, int status) {
  if (req->data) free(req->data);
  free(req);
}

/** @brief Send a WebSocket data frame (text or binary opcode) per RFC 6455
 * §5.2.
 *
 * Constructs a WebSocket frame with the FIN bit set (final fragment), the
 * specified opcode (0x01 for text, 0x02 for binary), and no masking
 * (server-to-client frames are unmasked per RFC 6455). Supports payload
 * lengths up to 2^64-1 using 7-bit, 16-bit, or 64-bit extended length
 * fields as appropriate.
 *
 * @param c       The request context (must have _internal_client set).
 * @param payload Frame payload data.
 * @param len     Payload length in bytes.
 * @param opcode  WebSocket opcode (0x01 for text, 0x02 for binary).
 * @note The payload is copied into a heap-allocated frame buffer which is
 *       freed by the write completion callback. This function returns
 *       immediately; the write happens asynchronously. */
void csilk_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len,
                   int opcode) {
  if (!c || !c->_internal_client) return;

  size_t header_len = 2;
  if (len > 125 && len <= 65535)
    header_len += 2;
  else if (len > 65535)
    header_len += 8;

  if (len > SIZE_MAX - header_len) return;

  uint8_t* frame = malloc(header_len + len);
  if (!frame) return;

  frame[0] = 0x80 | (opcode & 0x0F);
  if (len <= 125) {
    frame[1] = (uint8_t)len;
  } else if (len <= 65535) {
    frame[1] = 126;
    frame[2] = (uint8_t)((len >> 8) & 0xFF);
    frame[3] = (uint8_t)(len & 0xFF);
  } else {
    frame[1] = 127;
    for (int i = 0; i < 8; i++) {
      frame[2 + i] = (uint8_t)((len >> (56 - i * 8)) & 0xFF);
    }
  }
  memcpy(frame + header_len, payload, len);

  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req) {
    uv_buf_t buf = uv_buf_init((char*)frame, (unsigned int)(header_len + len));
    write_req->data = frame;
    // The first member of csilk_client_t is uv_tcp_t handle
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    uv_write(write_req, stream, &buf, 1, on_ws_write);
  } else {
    free(frame);
  }
}

/** @brief libuv write completion callback for WebSocket close frames.
 *
 * Frees the frame buffer and write request. Logs an error if the write
 * failed.
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (negative indicates error, logged to stderr). */
static void on_close_write(uv_write_t* req, int status) {
  if (status < 0) {
    CSILK_LOG_E("WS close write error: %s", uv_strerror(status));
  }
  if (req->data) free(req->data);
  free(req);
}

/** @brief Send a WebSocket close frame per RFC 6455 §5.5.1.
 *
 * Constructs and sends a close frame with an optional status code and reason
 * string. The payload is limited to 125 bytes (close frame + reason). After
 * sending, the connection is NOT immediately closed — the peer is expected
 * to respond with its own close frame.
 *
 * @param c           The request context.
 * @param status_code WebSocket close status code (e.g., 1000 for normal,
 *                    1001 for "going away"). Pass 0 to omit the status code.
 * @param reason      Human-readable reason string (may be NULL).
 * @note The frame is sent asynchronously. The connection should be closed
 *       by the caller after receiving the peer's close frame or on timeout. */
void csilk_ws_close(csilk_ctx_t* c, uint16_t status_code, const char* reason) {
  if (!c || !c->_internal_client) return;

  size_t reason_len = reason ? strlen(reason) : 0;
  size_t payload_len = (status_code > 0) ? (2 + reason_len) : reason_len;

  size_t header_len = 2;
  if (payload_len > 125) return;

  size_t frame_len = header_len + payload_len;
  uint8_t* frame = malloc(frame_len);
  if (!frame) return;

  frame[0] = 0x88;
  frame[1] = (uint8_t)payload_len;

  size_t offset = 2;
  if (status_code > 0) {
    frame[offset++] = (uint8_t)((status_code >> 8) & 0xFF);
    frame[offset++] = (uint8_t)(status_code & 0xFF);
  }
  if (reason && reason_len > 0) {
    memcpy(frame + offset, reason, reason_len);
  }

  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req) {
    uv_buf_t buf = uv_buf_init((char*)frame, (unsigned int)frame_len);
    write_req->data = frame;
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    uv_write(write_req, stream, &buf, 1, on_close_write);
  } else {
    free(frame);
  }
}

/** @brief Parse and dispatch an incoming WebSocket frame per RFC 6455 §5.2.
 *
 * Parses the frame header (FIN, opcode, mask, payload length), extracts
 * the payload, applies client-to-server masking if present, and dispatches
 * based on opcode:
 * - Opcode 0x08 (close): auto-responds with a close frame and closes the
 *   TCP connection.
 * - Other data opcodes: invokes the on_ws_message callback if set.
 *
 * @param c     The request context.
 * @param buf   Raw frame data received from the socket.
 * @param nread Number of bytes available in @p buf.
 * @note Only complete frames should be passed. Partial frames will be
 *       silently ignored (insufficient data check returns early).
 * @note The payload is heap-allocated, unmasked, and freed after dispatch. */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread) {
  if (nread < 2) return;

  // uint8_t fin = (buf[0] >> 7) & 0x01;
  uint8_t opcode = buf[0] & 0x0F;
  uint8_t masked = (buf[1] >> 7) & 0x01;
  uint64_t payload_len = buf[1] & 0x7F;

  size_t offset = 2;
  if (payload_len == 126) {
    if (nread < 4) return;
    payload_len = (buf[2] << 8) | buf[3];
    offset = 4;
  } else if (payload_len == 127) {
    if (nread < 10) return;
    payload_len = 0;
    for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | buf[2 + i];
    offset = 10;
  }

  uint8_t mask[4] = {0};
  if (masked) {
    if (nread < offset + 4) return;
    memcpy(mask, buf + offset, 4);
    offset += 4;
  }

  if (nread < offset + payload_len) return;

  uint8_t* payload = malloc(payload_len + 1);
  if (!payload) return;
  memcpy(payload, buf + offset, payload_len);

  if (masked) {
    for (uint64_t i = 0; i < payload_len; i++) {
      payload[i] ^= mask[i % 4];
    }
  }
  payload[payload_len] = '\0';

  if (opcode == 0x08) {
    uint16_t close_code = 1005;
    if (payload_len >= 2) {
      close_code = (uint16_t)((payload[0] << 8) | payload[1]);
    }
    csilk_ws_close(c, close_code, NULL);
    if (c->_internal_client) {
      uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
      if (!uv_is_closing((uv_handle_t*)stream)) {
        uv_close((uv_handle_t*)stream, NULL);
      }
    }
    free(payload);
    return;
  }

  if (c->on_ws_message) {
    c->on_ws_message(c, payload, (size_t)payload_len, opcode);
  }

  free(payload);
}
