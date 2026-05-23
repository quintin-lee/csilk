/**
 * @file websocket.c
 * @brief WebSocket handshake, send, and frame parsing implementation.
 * MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <uv.h>
#include "gin.h"
#include "gin_internal.h"

/** @brief WebSocket GUID for handshake key generation (RFC 6455). */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

void gin_ws_handshake(gin_ctx_t* c) {
    const char* key = gin_get_header(c, "Sec-WebSocket-Key");
    if (!key) {
        gin_json_error(c, 400, "Upgrade Required: Sec-WebSocket-Key missing");
        return;
    }

    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

    gin_sha1_ctx sha_ctx;
    uint8_t digest[20];
    gin_sha1_init(&sha_ctx);
    gin_sha1_update(&sha_ctx, (uint8_t*)combined, (uint32_t)strlen(combined));
    gin_sha1_final(&sha_ctx, digest);

    char accept_key[32];
    gin_base64_encode(digest, 20, accept_key);

    gin_set_header(c, "Upgrade", "websocket");
    gin_set_header(c, "Connection", "Upgrade");
    gin_set_header(c, "Sec-WebSocket-Accept", accept_key);
    
    gin_status(c, 101);
    c->is_websocket = 1;
}

/** @brief Write completion callback for WebSocket frame writes.
 * @param req Write request.
 * @param status Write status. */
static void on_ws_write(uv_write_t* req, int status) {
    if (req->data) free(req->data);
    free(req);
}

void gin_ws_send(gin_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    if (!c || !c->_internal_client) return;

    size_t header_len = 2;
    if (len > 125 && len <= 65535) header_len += 2;
    else if (len > 65535) header_len += 8;

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
        // The first member of gin_client_t is uv_tcp_t handle
        uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
        uv_write(write_req, stream, &buf, 1, on_ws_write);
    } else {
        free(frame);
    }
}

void gin_ws_parse_frame(gin_ctx_t* c, const uint8_t* buf, size_t nread) {
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
    
    if (c->on_ws_message) {
        c->on_ws_message(c, payload, (size_t)payload_len, opcode);
    }
    
    free(payload);
}
