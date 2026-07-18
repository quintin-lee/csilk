/**
 * @file h3.c
 * @brief HTTP/3 (RFC 9114) Frame Encoding/Decoding and RFC 9000 QUIC Varint implementation.
 */

#include "csilk/protocols/h3.h"
#include "csilk/csilk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t
csilk_h3_varint_encode(uint64_t val, uint8_t* out_buf)
{
    if (!out_buf) {
        return 0;
    }

    if (val < 64ULL) {
        /* 1-byte encoding: 2MSB = 00 */
        out_buf[0] = (uint8_t)(val & 0x3F);
        return 1;
    } else if (val < 16384ULL) {
        /* 2-byte encoding: 2MSB = 01 */
        out_buf[0] = (uint8_t)(0x40 | ((val >> 8) & 0x3F));
        out_buf[1] = (uint8_t)(val & 0xFF);
        return 2;
    } else if (val < 1073741824ULL) {
        /* 4-byte encoding: 2MSB = 10 */
        out_buf[0] = (uint8_t)(0x80 | ((val >> 24) & 0x3F));
        out_buf[1] = (uint8_t)((val >> 16) & 0xFF);
        out_buf[2] = (uint8_t)((val >> 8) & 0xFF);
        out_buf[3] = (uint8_t)(val & 0xFF);
        return 4;
    } else if (val <= 4611686018427387903ULL) {
        /* 8-byte encoding: 2MSB = 11 */
        out_buf[0] = (uint8_t)(0xC0 | ((val >> 56) & 0x3F));
        out_buf[1] = (uint8_t)((val >> 48) & 0xFF);
        out_buf[2] = (uint8_t)((val >> 40) & 0xFF);
        out_buf[3] = (uint8_t)((val >> 32) & 0xFF);
        out_buf[4] = (uint8_t)((val >> 24) & 0xFF);
        out_buf[5] = (uint8_t)((val >> 16) & 0xFF);
        out_buf[6] = (uint8_t)((val >> 8) & 0xFF);
        out_buf[7] = (uint8_t)(val & 0xFF);
        return 8;
    }
    return 0;
}

size_t
csilk_h3_varint_decode(const uint8_t* in_buf, size_t len, uint64_t* out_val)
{
    if (!in_buf || !out_val || len == 0) {
        return 0;
    }

    uint8_t prefix = (in_buf[0] >> 6) & 0x03;
    size_t  varint_len = 1ULL << prefix;

    if (len < varint_len) {
        return 0; /* Buffer truncated */
    }

    uint64_t val = in_buf[0] & 0x3F;
    for (size_t i = 1; i < varint_len; i++) {
        val = (val << 8) | in_buf[i];
    }
    *out_val = val;
    return varint_len;
}

size_t
csilk_h3_frame_encode(uint64_t       frame_type,
                      const uint8_t* payload,
                      size_t         payload_len,
                      uint8_t*       out_buf,
                      size_t         max_buf_len)
{
    if (!out_buf) {
        return 0;
    }

    uint8_t type_buf[8];
    size_t  type_len = csilk_h3_varint_encode(frame_type, type_buf);
    if (type_len == 0) {
        return 0;
    }

    uint8_t len_buf[8];
    size_t  len_bytes = csilk_h3_varint_encode((uint64_t)payload_len, len_buf);
    if (len_bytes == 0) {
        return 0;
    }

    size_t total_header_len = type_len + len_bytes;
    if (max_buf_len < total_header_len + payload_len) {
        return 0; /* Insufficient output capacity */
    }

    memcpy(out_buf, type_buf, type_len);
    memcpy(out_buf + type_len, len_buf, len_bytes);

    if (payload_len > 0 && payload) {
        memcpy(out_buf + total_header_len, payload, payload_len);
    }
    return total_header_len + payload_len;
}

int
csilk_h3_frame_decode(const uint8_t* in_buf,
                      size_t         in_len,
                      uint64_t*      out_type,
                      size_t*        out_payload_offset,
                      size_t*        out_payload_len)
{
    if (!in_buf || !out_type || !out_payload_offset || !out_payload_len) {
        return -1;
    }

    uint64_t ftype = 0;
    size_t   type_bytes = csilk_h3_varint_decode(in_buf, in_len, &ftype);
    if (type_bytes == 0) {
        return -1;
    }

    uint64_t flen = 0;
    size_t   len_bytes = csilk_h3_varint_decode(in_buf + type_bytes, in_len - type_bytes, &flen);
    if (len_bytes == 0) {
        return -1;
    }

    size_t header_len = type_bytes + len_bytes;
    if (in_len < header_len + (size_t)flen) {
        return -1; /* Incomplete frame payload */
    }

    *out_type = ftype;
    *out_payload_offset = header_len;
    *out_payload_len = (size_t)flen;
    return 0;
}

void
csilk_h3_inject_alt_svc_header(csilk_ctx_t* c, int h3_port)
{
    if (!c) {
        return;
    }
    int  port = h3_port > 0 ? h3_port : 443;
    char alt_svc[64];
    snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
    csilk_set_header(c, "Alt-Svc", alt_svc);
}
