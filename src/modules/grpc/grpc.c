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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { CSILK_GRPC_MAX_FRAME_LEN = 64 * 1024 * 1024 };

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

    if (payload_len > CSILK_GRPC_MAX_FRAME_LEN || (size_t)payload_len > len - 5) {
        return -1; /* incomplete or oversized frame */
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
    if (!out_buf || payload_len > CSILK_GRPC_MAX_FRAME_LEN || payload_len > (size_t)INT_MAX - 5 ||
        out_buf_cap < payload_len + 5) {
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

    if (out_status) {
        *out_status = 0; /* default OK */
    }
    if (out_message) {
        *out_message = NULL;
    }

    for (size_t i = 0; i < nheaders; i++) {
        const char* key = headers[i].key ? headers[i].key : "";
        const char* val = headers[i].val ? headers[i].val : "";

        if (strcasecmp(key, "grpc-status") == 0 && out_status) {
            *out_status = atoi(val);
        } else if (strcasecmp(key, "grpc-message") == 0 && out_message) {
            free(*out_message);
            *out_message = strdup(val);
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
            free(*out_content_type);
            *out_content_type = strdup(val[0] ? val : "application/grpc");
        } else if (strcasecmp(key, "grpc-timeout") == 0 && out_timeout_ms) {
            /* gRPC timeout format: "<value><unit>" where unit is
             * m (milliseconds), M (3.6e8 ms), s, S, h, or H */
            char   buf[32];
            size_t vlen = strlen(val);
            if (vlen >= 2 && vlen < sizeof(buf)) {
                memcpy(buf, val, vlen);
                buf[vlen] = '\0';
                char unit = buf[vlen - 1];
                buf[vlen - 1] = '\0';
                char*     endptr = NULL;
                long long value = strtoll(buf, &endptr, 10);
                if (endptr != buf && *endptr == '\0' && value >= 0) {
                    int64_t v64 = (int64_t)value;
                    switch (unit) {
                    case 'm':
                        *out_timeout_ms = v64;
                        break;
                    case 'M':
                        *out_timeout_ms =
                            (v64 > INT64_MAX / 360000000000LL) ? INT64_MAX : v64 * 360000000000LL;
                        break;
                    case 's':
                        *out_timeout_ms = (v64 > INT64_MAX / 1000LL) ? INT64_MAX : v64 * 1000LL;
                        break;
                    case 'S':
                        *out_timeout_ms =
                            (v64 > INT64_MAX / 360000000000LL) ? INT64_MAX : v64 * 360000000000LL;
                        break;
                    case 'h':
                    case 'H':
                        *out_timeout_ms =
                            (v64 > INT64_MAX / 3600000LL) ? INT64_MAX : v64 * 3600000LL;
                        break;
                    default:
                        *out_timeout_ms = v64;
                        break;
                    }
                }
            }
        }
    }

    return 0;
}

/* ===================================================================
 * gRPC Streaming
 * =================================================================== */

struct csilk_grpc_stream_s {
    csilk_grpc_stream_type_t type;
    csilk_grpc_on_message_cb on_msg;
    void*                    user_data;
    int                      closed;
    int                      half_closed;
    uint8_t*                 rbuf;
    size_t                   rbuf_len;
    size_t                   rbuf_cap;
    uint32_t                 expected_len;
    int                      in_header;
    size_t                   header_pos;
};

csilk_grpc_stream_t*
csilk_grpc_stream_new(csilk_grpc_stream_type_t type,
                      csilk_grpc_on_message_cb on_msg,
                      void*                    user_data)
{
    csilk_grpc_stream_t* s = calloc(1, sizeof(csilk_grpc_stream_t));
    if (!s) {
        return NULL;
    }

    s->type = type;
    s->on_msg = on_msg;
    s->user_data = user_data;
    s->closed = 0;
    s->half_closed = 0;
    s->in_header = 1;
    s->header_pos = 0;
    s->expected_len = 0;

    s->rbuf_cap = 4096;
    s->rbuf = malloc(s->rbuf_cap);
    if (!s->rbuf) {
        free(s);
        return NULL;
    }
    s->rbuf_len = 0;

    return s;
}

int
csilk_grpc_stream_send(csilk_grpc_stream_t* stream,
                       const uint8_t*       payload,
                       size_t               payload_len,
                       int                  is_last)
{
    if (!stream || stream->closed) {
        return -1;
    }

    (void)payload;
    (void)payload_len;

    if (is_last) {
        stream->half_closed = 1;
        if (stream->type == CSILK_GRPC_STREAM_UNARY ||
            stream->type == CSILK_GRPC_STREAM_CLIENT_STREAMING) {
            stream->closed = 1;
        }
    }

    return 0;
}

int
csilk_grpc_stream_feed(csilk_grpc_stream_t* stream, const uint8_t* data, size_t len)
{
    if (!stream || stream->closed || !data) {
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        if (stream->in_header) {
            stream->header_pos++;
            if (stream->rbuf_len + 1 > stream->rbuf_cap) {
                /* rbuf_cap may start at 0; a 0 * 2 realloc(0) is UB/returns NULL.
                 * Seed a minimum capacity before doubling. */
                size_t   new_cap = (stream->rbuf_cap > 0) ? (stream->rbuf_cap * 2) : 256;
                uint8_t* tmp = realloc(stream->rbuf, new_cap);
                if (!tmp) {
                    return -1;
                }
                stream->rbuf = tmp;
                stream->rbuf_cap = new_cap;
            }
            stream->rbuf[stream->rbuf_len++] = byte;

            if (stream->header_pos >= 5) {
                stream->expected_len = ((uint32_t)stream->rbuf[1] << 24) |
                                       ((uint32_t)stream->rbuf[2] << 16) |
                                       ((uint32_t)stream->rbuf[3] << 8) | (uint32_t)stream->rbuf[4];
                if (stream->expected_len > CSILK_GRPC_MAX_FRAME_LEN) {
                    stream->in_header = 1;
                    stream->header_pos = 0;
                    stream->expected_len = 0;
                    stream->rbuf_len = 0;
                    return -1;
                }
                if (stream->expected_len == 0) {
                    if (stream->on_msg) {
                        stream->on_msg(stream, stream->rbuf + 5, 0, stream->user_data);
                    }
                    stream->in_header = 1;
                    stream->header_pos = 0;
                    stream->rbuf_len = 0;
                } else {
                    stream->in_header = 0;
                    stream->header_pos = 0;
                }
            }
            continue;
        }

        if (stream->rbuf_len + 1 > stream->rbuf_cap) {
            if (stream->rbuf_cap > (size_t)CSILK_GRPC_MAX_FRAME_LEN + 5) {
                return -1;
            }
            size_t   new_cap = stream->rbuf_cap * 2;
            uint8_t* tmp = realloc(stream->rbuf, new_cap);
            if (!tmp) {
                return -1;
            }
            stream->rbuf = tmp;
            stream->rbuf_cap = new_cap;
        }
        stream->rbuf[stream->rbuf_len++] = byte;

        size_t payload_start = 5;
        if (stream->rbuf_len - payload_start >= stream->expected_len) {
            if (stream->on_msg) {
                stream->on_msg(
                    stream, stream->rbuf + payload_start, stream->expected_len, stream->user_data);
            }
            stream->in_header = 1;
            stream->header_pos = 0;
            stream->expected_len = 0;
            stream->rbuf_len = 0;
        }
    }

    return 0;
}

void
csilk_grpc_stream_end(csilk_grpc_stream_t* stream)
{
    if (stream) {
        stream->closed = 1;
    }
}

int
csilk_grpc_stream_closed(const csilk_grpc_stream_t* stream)
{
    if (!stream) {
        return 1;
    }
    return stream->closed;
}

void
csilk_grpc_stream_destroy(csilk_grpc_stream_t* stream)
{
    if (!stream) {
        return;
    }
    free(stream->rbuf);
    free(stream);
}
