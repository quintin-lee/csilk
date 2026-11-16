/*
 * src/protocols/grpc.c
 *
 * gRPC (gRPC over HTTP/2) frame parsing and trailer handling.
 *
 * Implements the gRPC wire format:
 *   - 5-byte frame header: 1 byte compression flag + 4 byte big-endian length
 *   - Frame payload: length bytes of (potentially compressed) protobuf data
 *   - Trailer: grpc-status, grpc-message, and custom metadata
 *
 * @copyright MIT License
 */

#include "csilk/protocols/grpc.h"

#include <stdlib.h>
#include <string.h>

/**
 * Parse a gRPC frame from a buffer.
 *
 * @param data   Input buffer.
 * @param len    Length of data.
 * @param out_payload [out] Pointer to frame payload (within data).
 * @param out_payload_len [out] Payload length.
 * @param out_compressed [out] 1 if compressed, 0 if not.
 * @return 0 on success, -1 if buffer is too short for a complete frame.
 */
int
csilk_grpc_frame_parse(const uint8_t*  data,
                       size_t          len,
                       const uint8_t** out_payload,
                       size_t*         out_payload_len,
                       int*            out_compressed)
{
    if (!data || len < 5 || !out_payload || !out_payload_len) {
        return -1;
    }

    int      compressed = data[0];
    uint32_t payload_len = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 8) | (uint32_t)data[4];

    if (5 + payload_len > len) {
        return -1; /* incomplete frame */
    }

    *out_payload = data + 5;
    *out_payload_len = payload_len;
    if (out_compressed) {
        *out_compressed = compressed;
    }

    return 0;
}

/**
 * Encode a gRPC frame into a buffer.
 *
 * @param payload     Frame payload (protobuf or compressed data).
 * @param payload_len Payload length.
 * @param compressed  1 if payload is compressed, 0 if not.
 * @param out_buf     Destination buffer (must be >= payload_len + 5).
 * @param out_buf_cap Capacity of out_buf.
 * @return Total frame length (payload_len + 5), or -1 on error.
 */
int
csilk_grpc_frame_encode_ex(const uint8_t* payload,
                           size_t         payload_len,
                           int            compressed,
                           uint8_t*       out_buf,
                           size_t         out_buf_cap)
{
    if (!out_buf || out_buf_cap < payload_len + 5) {
        return -1;
    }

    out_buf[0] = (uint8_t)(compressed ? 1 : 0);
    uint32_t len_be = (uint32_t)payload_len;
    out_buf[1] = (uint8_t)((len_be >> 24) & 0xFF);
    out_buf[2] = (uint8_t)((len_be >> 16) & 0xFF);
    out_buf[3] = (uint8_t)((len_be >> 8) & 0xFF);
    out_buf[4] = (uint8_t)(len_be & 0xFF);

    if (payload_len > 0 && payload) {
        memcpy(out_buf + 5, payload, payload_len);
    }

    return (int)(payload_len + 5);
}

/**
 * Parse gRPC trailers from a header array.
 *
 * gRPC trailers are HTTP/2 headers that come after the DATA stream ends.
 * The critical ones are:
 *   - grpc-status: integer status code (0 = OK)
 *   - grpc-message: human-readable error message
 *
 * @param headers      Array of key-value pairs.
 * @param nheaders     Number of headers.
 * @param out_status   [out] gRPC status code.
 * @param out_message  [out] Allocated error message string (caller frees), or NULL.
 * @return 0 on success, -1 if no headers.
 */
int
csilk_grpc_trailer_parse(const csilk_kv_t* headers,
                         size_t            nheaders,
                         int*              out_status,
                         char**            out_message)
{
    if (!headers || nheaders == 0) {
        return -1;
    }

    *out_status = 0; /* default OK */
    if (out_message) {
        *out_message = NULL;
    }

    for (size_t i = 0; i < nheaders; i++) {
        const char* key = headers[i].key ? headers[i].key : "";
        const char* val = headers[i].val ? headers[i].val : "";

        if (strcasecmp(key, "grpc-status") == 0) {
            *out_status = atoi(val);
        } else if (strcasecmp(key, "grpc-message") == 0 && out_message) {
            *out_message = strdup(val ? val : "");
        }
    }

    return 0;
}

/**
 * Parse gRPC metadata (request headers) from a header array.
 *
 * Maps gRPC metadata headers to csilk_kv_t entries:
 *   - content-type, grpc-timeout, and custom metadata
 *
 * @param headers      Array of key-value pairs.
 * @param nheaders     Number of headers.
 * @param out_content_type [out] Allocated content-type string, or NULL.
 * @param out_timeout_ms   [out] Timeout in milliseconds (-1 = none).
 * @return 0 on success, -1 if no headers.
 */
int
csilk_grpc_metadata_parse(const csilk_kv_t* headers,
                          size_t            nheaders,
                          char**            out_content_type,
                          int64_t*          out_timeout_ms)
{
    if (!headers || nheaders == 0) {
        return -1;
    }

    if (out_content_type) {
        *out_content_type = NULL;
    }
    if (out_timeout_ms) {
        *out_timeout_ms = -1;
    }

    for (size_t i = 0; i < nheaders; i++) {
        const char* key = headers[i].key ? headers[i].key : "";
        const char* val = headers[i].val ? headers[i].val : "";

        if (strcasecmp(key, "content-type") == 0 && out_content_type) {
            *out_content_type = strdup(val ? val : "application/grpc");
        } else if (strcasecmp(key, "grpc-timeout") == 0 && out_timeout_ms) {
            /* gRPC timeout format: "<value><unit>" where unit is
             * m (milliseconds), M (3.6e8 ms), s, S, h, or H */
            char   buf[32];
            size_t vlen = strlen(val);
            if (vlen < sizeof(buf)) {
                memcpy(buf, val, vlen);
                buf[vlen] = '\0';
            }

            int64_t value = atoll(buf);
            char    unit = buf[vlen - 1];
            switch (unit) {
            case 'm':
                *out_timeout_ms = value;
                break;
            case 'M':
                *out_timeout_ms = value * 360000000000LL; /* 3.6e8 ms */
                break;
            case 's':
                *out_timeout_ms = value * 1000;
                break;
            case 'S':
                *out_timeout_ms = value * 360000000000LL;
                break;
            case 'h':
            case 'H':
                *out_timeout_ms = value * 3600000;
                break;
            default:
                *out_timeout_ms = value;
                break;
            }
        }
    }

    return 0;
}
