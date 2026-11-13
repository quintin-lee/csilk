/**
 * @file websocket.c
 * @brief WebSocket handshake, frame encoding, and frame parsing implementation.
 *
 * Architecture:
 *   The WebSocket implementation follows RFC 6455. The handshake
 *   (csilk_ws_handshake) performs the HTTP upgrade using SHA-1 + Base64
 *   on the Sec-WebSocket-Key. Frame encoding (csilk_ws_send) constructs a
 *   raw WebSocket frame buffer per §5.2 and writes it via the I/O backend.
 *   parsing (csilk_ws_parse_frame) reads a complete frame from a buffer,
 *   unmasks client-to-server payloads, and dispatches by opcode. Close
 *   frames (csilk_ws_close) are encoded with optional status code + reason
 *   and sent asynchronously; the underlying TCP handle is closed after
 *   receiving the peer's close frame.
 *
 * Wire format (RFC 6455 §5.2, 1 byte = 8 bits):
 *   Byte 0: [FIN=1][RSV=000][opcode=xxxx]
 *   Byte 1: [MASK=1][payload len=xxxxxxx]
 *   Bytes 2+ : Extended payload length (2 or 8 bytes, if len==126 or 127)
 *   Bytes 2/4/10+: Masking key (4 bytes, if MASK==1)
 *   Remaining: Payload data (XOR-decoded with masking key if MASK==1)
 *
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk/core/internal.h"
#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"

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
void
csilk_ws_handshake(csilk_ctx_t* c)
{
    const char* key = csilk_get_header(c, "Sec-WebSocket-Key");
    if (!key) {
        csilk_json_error(
            c, CSILK_STATUS_BAD_REQUEST, "Upgrade Required: Sec-WebSocket-Key missing");
        return;
    }

    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

    csilk_sha1_ctx sha_ctx;
    uint8_t        digest[20];
    csilk_sha1_init(&sha_ctx);
    csilk_sha1_update(&sha_ctx, (uint8_t*)combined, (uint32_t)strlen(combined));
    csilk_sha1_final(&sha_ctx, digest);

    char accept_key[32];
    csilk_base64_encode(digest, 20, accept_key);
    explicit_bzero(digest, sizeof(digest));

    csilk_set_header(c, "Upgrade", "websocket");
    csilk_set_header(c, "Connection", "Upgrade");
    csilk_set_header(c, "Sec-WebSocket-Accept", accept_key);
    explicit_bzero(accept_key, sizeof(accept_key));

    csilk_status(c, CSILK_STATUS_SWITCHING_PROTOCOLS);
    csilk_ctx_set_websocket(c, 1);
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client) {
        csilk_conn_set_state(client, CSILK_CONN_STREAMING);
    }
}

/** @brief Write completion callback for WebSocket frame sends.
 *
 * Frees the frame buffer and the write request structure. Errors are
 * silently ignored.
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (ignored). */
static void
on_ws_write(csilk_io_write_t* req, int status)
{
    (void)status;
    csilk_client_t* client = NULL;
    if (req->handle) {
        client = (csilk_client_t*)req->handle->data;
    }
    if (req->data) {
        free(req->data);
    }
    free(req);

    if (client) {
        _csilk_check_and_trigger_drain(client);
        _csilk_client_pending_io_dec(client);
        csilk_client_unref(client);
    }
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
int
csilk_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
    if (!c) {
        return -1;
    }

    if (c->on_ws_send) {
        c->on_ws_send(c, payload, len, opcode);
    }

    void* internal_client = _csilk_get_internal_client(c);
    if (!internal_client) {
        return -1;
    }

    csilk_client_t* cl = (csilk_client_t*)internal_client;
    csilk_conn_set_state(cl, CSILK_CONN_STREAMING);
    size_t q = _csilk_client_get_write_queue_size(cl);

    size_t header_len = 2;
    if (len > 125 && len <= 65535) {
        header_len += 2;
    } else if (len > 65535) {
        header_len += 8;
    }

    if (len > SIZE_MAX - header_len) {
        return -1;
    }

    size_t frame_len = header_len + len;
    if (c->max_write_buffer_size > 0 && q + frame_len > c->max_write_buffer_size) {
        CSILK_LOG_E("WebSocket: max write buffer exceeded (%zu + %zu > %zu)",
                    q,
                    frame_len,
                    c->max_write_buffer_size);
        return -1;
    }

    uint8_t* frame = malloc(frame_len);
    if (!frame) {
        return -1;
    }

    /* Byte 0: FIN bit (0x80) OR'd with the opcode nibble (lower 4 bits).
   *   FIN=1 signals the final fragment of a message (fragmentation not used
   *   in this implementation).  RSV bits (0x70) remain 0 since no extension
   *   is negotiated.  Opcode: 0x1 = text, 0x2 = binary, 0x8 = close. */
    frame[0] = 0x80 | (opcode & 0x0F);

    /* Byte 1: MASK=0 (server-to-client frames are unmasked per RFC 6455 §5.1)
   *   plus payload length in the lower 7 bits.  Extended length encoding:
   *     len <= 125:   stored directly in byte 1
   *     126..65535:   byte 1 = 126, next 2 bytes = big-endian len
   *     > 65535:      byte 1 = 127, next 8 bytes = big-endian len
   *   This is a variable-length integer encoding (similar to QUIC varint). */
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

    csilk_conn_set_state(cl, CSILK_CONN_STREAMING);
    _csilk_send_data_owned(c, (char*)frame, frame_len);

    q = _csilk_client_get_write_queue_size(cl);
    if (c->write_high_water_mark > 0 && q >= c->write_high_water_mark) {
        c->write_paused = 1;
        return 0;
    }

    return 1;
}

void
csilk_ws_close(csilk_ctx_t* c, uint16_t status_code, const char* reason)
{
    void* internal_client = _csilk_get_internal_client(c);
    if (!c || !internal_client) {
        return;
    }

    size_t reason_len = reason ? strlen(reason) : 0;
    size_t payload_len = (status_code > 0) ? (2 + reason_len) : reason_len;

    size_t header_len = 2;
    if (payload_len > 125) {
        return;
    }

    size_t   frame_len = header_len + payload_len;
    uint8_t* frame = malloc(frame_len);
    if (!frame) {
        return;
    }

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

    csilk_client_t* cl = (csilk_client_t*)internal_client;
    csilk_conn_set_state(cl, CSILK_CONN_STREAMING);
    _csilk_send_data_owned(c, (char*)frame, frame_len);
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
void
csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread)
{
    if (!c) {
        return;
    }

    if (buf && nread > 0) {
        if (c->ws_buf_len + nread > c->ws_buf_cap) {
            size_t new_cap = c->ws_buf_cap ? c->ws_buf_cap * 2 : 4096;
            while (new_cap < c->ws_buf_len + nread) {
                new_cap *= 2;
            }
            uint8_t* new_buf = realloc(c->ws_buf, new_cap);
            if (!new_buf) {
                return;
            }
            c->ws_buf = new_buf;
            c->ws_buf_cap = new_cap;
        }
        memcpy(c->ws_buf + c->ws_buf_len, buf, nread);
        c->ws_buf_len += nread;
    }

    while (c->ws_buf_len >= 2) {
        uint8_t* cur = c->ws_buf;
        uint8_t  opcode = cur[0] & 0x0F;
        uint8_t  masked = (cur[1] >> 7) & 0x01;
        uint64_t payload_len = cur[1] & 0x7F;

        size_t offset = 2;
        if (payload_len == 126) {
            if (c->ws_buf_len < 4) {
                break;
            }
            payload_len = ((uint64_t)cur[2] << 8) | cur[3];
            offset = 4;
        } else if (payload_len == 127) {
            if (c->ws_buf_len < 10) {
                break;
            }
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | cur[2 + i];
            }
            offset = 10;
        }

        uint8_t mask[4] = {0};
        if (masked) {
            if (c->ws_buf_len < offset + 4) {
                break;
            }
            memcpy(mask, cur + offset, 4);
            offset += 4;
        }

        /* Check against integer overflow and enforce maximum frame payload size limit (CWE-190, CWE-122) */
        enum { CSILK_WS_MAX_FRAME_PAYLOAD = 64 * 1024 * 1024 };
        if (payload_len > CSILK_WS_MAX_FRAME_PAYLOAD || payload_len > SIZE_MAX - offset - 1) {
            CSILK_LOG_W("WebSocket: Frame payload length (%llu) exceeds maximum limit",
                        (unsigned long long)payload_len);
            c->ws_buf_len = 0;
            return;
        }

        size_t total_frame_len = offset + (size_t)payload_len;
        if (c->ws_buf_len < total_frame_len) {
            break;
        }

        uint8_t* payload = malloc((size_t)payload_len + 1);
        if (!payload) {
            return;
        }
        memcpy(payload, cur + offset, (size_t)payload_len);

        if (masked) {
            for (uint64_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask[i % 4];
            }
        }
        payload[payload_len] = '\0';

        size_t remaining = c->ws_buf_len - total_frame_len;
        if (remaining > 0) {
            memmove(c->ws_buf, c->ws_buf + total_frame_len, remaining);
        }
        c->ws_buf_len = remaining;

        if (opcode == 0x08) {
            uint16_t close_code = 1005;
            if (payload_len >= 2) {
                close_code = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
            }
            csilk_ws_close(c, close_code, NULL);
            void* close_client = _csilk_get_internal_client(c);
            if (close_client) {
                csilk_client_t*    cl = (csilk_client_t*)close_client;
                csilk_io_stream_t* stream = (csilk_io_stream_t*)&cl->handle;
                if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                    csilk_io_close((csilk_io_handle_t*)stream, NULL);
                }
            }
            free(payload);
            c->ws_buf_len = 0;
            return;
        } else if (opcode == 0x09) {
            csilk_ws_send(c, payload, (size_t)payload_len, 0x0A);
        } else if (opcode == 0x0A) {
            /* Pong frame - do nothing */
        } else {
            void (*on_ws_msg)(csilk_ctx_t*, const uint8_t*, size_t, int) =
                csilk_get_on_ws_message(c);
            if (on_ws_msg) {
                on_ws_msg(c, payload, (size_t)payload_len, opcode);
            }
        }

        free(payload);
    }
}
