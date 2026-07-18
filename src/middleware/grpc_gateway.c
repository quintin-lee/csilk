/**
 * @file grpc_gateway.c
 * @brief HTTP/JSON <-> gRPC Transcoding Gateway middleware implementation.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
csilk_grpc_frame_encode(const uint8_t* proto_payload,
                        size_t         payload_len,
                        uint8_t*       out_buf,
                        size_t         max_buf_len)
{
    if (!out_buf || max_buf_len < payload_len + 5) {
        return -1;
    }
    // 1st byte: Compressed-Flag (0 = uncompressed)
    out_buf[0] = 0x00;

    // Bytes 1..4: Message Length in Big-Endian (Network Byte Order)
    uint32_t len_be = (uint32_t)payload_len;
    out_buf[1] = (uint8_t)((len_be >> 24) & 0xFF);
    out_buf[2] = (uint8_t)((len_be >> 16) & 0xFF);
    out_buf[3] = (uint8_t)((len_be >> 8) & 0xFF);
    out_buf[4] = (uint8_t)(len_be & 0xFF);

    if (payload_len > 0 && proto_payload) {
        memcpy(out_buf + 5, proto_payload, payload_len);
    }
    return (int)(payload_len + 5);
}

void
csilk_grpc_gateway_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    const char* ct = csilk_get_header(c, "content-type");
    const char* gw_hdr = csilk_get_header(c, "x-grpc-gateway");

    int is_grpc_json =
        (ct && strstr(ct, "application/grpc")) || (gw_hdr && strcmp(gw_hdr, "1") == 0);

    if (is_grpc_json) {
        size_t      body_len = 0;
        const char* body = csilk_get_body(c, &body_len);

        csilk_set_header(c, "content-type", "application/grpc");
        csilk_set_header(c, "grpc-status", "0"); // 0 = OK

        if (body && body_len > 0) {
            uint8_t frame_buf[4096];
            int     frame_len = csilk_grpc_frame_encode(
                (const uint8_t*)body, body_len, frame_buf, sizeof(frame_buf));
            if (frame_len > 0) {
                csilk_set_response_body(c, (const char*)frame_buf, (size_t)frame_len, 1);
            }
        }
    }

    csilk_next(c);
}
