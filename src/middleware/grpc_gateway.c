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
#include "csilk/protocols/grpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Encapsulate raw Protobuf payload into a standard 5-byte gRPC frame.
 *
 * Delegates to csilk_grpc_frame_encode_ex() with compressed = 0 so both entry
 * points share identical length bounds and big-endian framing logic.
 *
 * @param proto_payload Raw binary Protobuf byte payload.
 * @param payload_len Byte length of @p proto_payload.
 * @param[out] out_buf Destination frame buffer (must be at least payload_len + 5 bytes).
 * @param max_buf_len Capacity of @p out_buf.
 * @return Total gRPC frame length in bytes (payload_len + 5), or -1 if invalid or insufficient capacity.
 */
int
csilk_grpc_frame_encode(const uint8_t* proto_payload,
                        size_t         payload_len,
                        uint8_t*       out_buf,
                        size_t         max_buf_len)
{
    return csilk_grpc_frame_encode_ex(proto_payload, payload_len, 0, out_buf, max_buf_len);
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
            uint8_t* frame_buf = csilk_arena_alloc(csilk_get_arena(c), body_len + 5);
            if (frame_buf) {
                int frame_len = csilk_grpc_frame_encode(
                    (const uint8_t*)body, body_len, frame_buf, body_len + 5);
                if (frame_len > 0) {
                    csilk_set_response_body(c, (const char*)frame_buf, (size_t)frame_len, 0);
                }
            }
        }
    }

    /* Pass control to next handler in chain */
    csilk_next(c);
}
