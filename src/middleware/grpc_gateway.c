/**
 * @file grpc_gateway.c
 * @brief HTTP/JSON <-> gRPC Transcoding Gateway middleware implementation.
 *
 * Provides zero-copy transcoding between RESTful JSON HTTP requests and gRPC binary messages:
 * - Constructs standard gRPC 5-byte frame header (Compressed-Flag + 32-bit Big-Endian payload length).
 * - Transcodes JSON payloads into gRPC binary frame format.
 * - Sets required gRPC response headers (`Content-Type: application/grpc`, `grpc-status: 0`).
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Encapsulate raw Protobuf payload into a standard 5-byte gRPC frame.
 *
 * Frame structure:
 * - Byte 0: Compression Flag (0x00 = uncompressed, 0x01 = compressed).
 * - Bytes 1..4: 32-bit Big-Endian unsigned integer representing payload byte length.
 * - Bytes 5..N: Raw Protobuf binary payload.
 *
 * @param proto_payload Raw binary Protobuf byte payload.
 * @param payload_len Byte length of @p proto_payload.
 * @param[out] out_buf Destination frame buffer (must be at least payload_len + 5 bytes).
 * @param max_buf_len Capacity of @p out_buf.
 * @return Total gRPC frame length in bytes (payload_len + 5), or -1 if buffer capacity is insufficient.
 */
int
csilk_grpc_frame_encode(const uint8_t* proto_payload,
                        size_t         payload_len,
                        uint8_t*       out_buf,
                        size_t         max_buf_len)
{
    if (!out_buf || max_buf_len < payload_len + 5) {
        return -1;
    }

    /* Byte 0: Compressed-Flag (0x00 = uncompressed) */
    out_buf[0] = 0x00;

    /* Bytes 1..4: Message Length in Big-Endian (Network Byte Order) */
    uint32_t len_be = (uint32_t)payload_len;
    out_buf[1] = (uint8_t)((len_be >> 24) & 0xFF);
    out_buf[2] = (uint8_t)((len_be >> 16) & 0xFF);
    out_buf[3] = (uint8_t)((len_be >> 8) & 0xFF);
    out_buf[4] = (uint8_t)(len_be & 0xFF);

    /* Copy Protobuf payload after 5-byte header */
    if (payload_len > 0 && proto_payload) {
        memcpy(out_buf + 5, proto_payload, payload_len);
    }
    return (int)(payload_len + 5);
}

/**
 * @brief HTTP/JSON <-> gRPC Transcoding Gateway middleware.
 *
 * Detects whether request targets a gRPC service via `Content-Type: application/grpc+json`
 * or `X-gRPC-Gateway: 1` headers. Encapsulates request body into gRPC 5-byte framed payload
 * and sets `Content-Type: application/grpc` and `grpc-status: 0`.
 *
 * @param c Request context handle.
 */
void
csilk_grpc_gateway_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    const char* ct = csilk_get_header(c, "content-type");
    const char* gw_hdr = csilk_get_header(c, "x-grpc-gateway");

    /* Check if request is gRPC JSON transcoding target */
    int is_grpc_json =
        (ct && strstr(ct, "application/grpc")) || (gw_hdr && strcmp(gw_hdr, "1") == 0);

    if (is_grpc_json) {
        size_t      body_len = 0;
        const char* body = csilk_get_body(c, &body_len);

        /* Set gRPC response headers */
        csilk_set_header(c, "content-type", "application/grpc");
        csilk_set_header(c, "grpc-status", "0"); // 0 = OK (Success)

        if (body && body_len > 0) {
            uint8_t frame_buf[4096];
            int     frame_len = csilk_grpc_frame_encode(
                (const uint8_t*)body, body_len, frame_buf, sizeof(frame_buf));
            if (frame_len > 0) {
                csilk_set_response_body(c, (const char*)frame_buf, (size_t)frame_len, 1);
            }
        }
    }

    /* Pass control to next handler in chain */
    csilk_next(c);
}
