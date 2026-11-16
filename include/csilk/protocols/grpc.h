/**
 * @file grpc.h
 * @brief gRPC frame parsing, encoding, and trailer/metadata handling.
 *
 * Implements the gRPC wire format over HTTP/2:
 *   - 5-byte frame header (compression flag + 32-bit big-endian length)
 *   - Trailer parsing (grpc-status, grpc-message)
 *   - Metadata parsing (content-type, grpc-timeout)
 *
 * @copyright MIT License
 */

#ifndef CSILK_PROTOCOLS_GRPC_H
#define CSILK_PROTOCOLS_GRPC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Key-value pair for gRPC metadata/trailer parsing. */
typedef struct {
    const char* key;
    const char* val;
} csilk_kv_t;

/**
 * @brief Parse a gRPC frame from a buffer.
 *
 * @param data          Input buffer.
 * @param len           Length of data.
 * @param out_payload   [out] Pointer to frame payload (within data).
 * @param out_payload_len [out] Payload length.
 * @param out_compressed [out] 1 if compressed, 0 if not (may be NULL).
 * @return 0 on success, -1 if buffer is too short.
 */
int csilk_grpc_frame_parse(const uint8_t*  data,
                           size_t          len,
                           const uint8_t** out_payload,
                           size_t*         out_payload_len,
                           int*            out_compressed);

/**
 * @brief Encode a gRPC frame into a buffer.
 *
 * @param payload      Frame payload.
 * @param payload_len  Payload length.
 * @param compressed   1 if payload is compressed, 0 if not.
 * @param out_buf      Destination buffer.
 * @param out_buf_cap  Capacity of out_buf.
 * @return Total frame length (payload_len + 5), or -1 on error.
 */
int csilk_grpc_frame_encode_ex(const uint8_t* payload,
                               size_t         payload_len,
                               int            compressed,
                               uint8_t*       out_buf,
                               size_t         out_buf_cap);

/**
 * @brief Parse gRPC trailers (grpc-status, grpc-message).
 *
 * @param headers      Array of key-value pairs.
 * @param nheaders     Number of headers.
 * @param out_status   [out] gRPC status code.
 * @param out_message  [out] Allocated error message (caller frees), or NULL.
 * @return 0 on success, -1 if no headers.
 */
int csilk_grpc_trailer_parse(const csilk_kv_t* headers,
                             size_t            nheaders,
                             int*              out_status,
                             char**            out_message);

/**
 * @brief Parse gRPC metadata (request headers).
 *
 * @param headers         Array of key-value pairs.
 * @param nheaders        Number of headers.
 * @param out_content_type [out] Allocated content-type string, or NULL.
 * @param out_timeout_ms   [out] Timeout in ms (-1 = none).
 * @return 0 on success, -1 if no headers.
 */
int csilk_grpc_metadata_parse(const csilk_kv_t* headers,
                              size_t            nheaders,
                              char**            out_content_type,
                              int64_t*          out_timeout_ms);

/* --- gRPC Streaming --- */

/** @brief gRPC stream type. */
typedef enum {
    CSILK_GRPC_STREAM_UNARY = 0,            /**< One request, one response. */
    CSILK_GRPC_STREAM_CLIENT_STREAMING = 1, /**< Many requests, one response. */
    CSILK_GRPC_STREAM_SERVER_STREAMING = 2, /**< One request, many responses. */
    CSILK_GRPC_STREAM_BIDI_STREAMING = 3    /**< Many requests, many responses. */
} csilk_grpc_stream_type_t;

/** @brief Opaque handle for a gRPC stream. */
typedef struct csilk_grpc_stream_s csilk_grpc_stream_t;

/** @brief Callback for receiving a gRPC stream message. */
typedef void (*csilk_grpc_on_message_cb)(csilk_grpc_stream_t* stream,
                                         const uint8_t*       data,
                                         size_t               len,
                                         void*                user_data);

/**
 * @brief Create a gRPC stream of the given type.
 */
csilk_grpc_stream_t* csilk_grpc_stream_new(csilk_grpc_stream_type_t type,
                                           csilk_grpc_on_message_cb on_msg,
                                           void*                    user_data);

/**
 * @brief Send a gRPC frame on a stream.
 */
int csilk_grpc_stream_send(csilk_grpc_stream_t* stream,
                           const uint8_t*       payload,
                           size_t               payload_len,
                           int                  is_last);

/**
 * @brief Feed received bytes into a stream (for server-side).
 */
int csilk_grpc_stream_feed(csilk_grpc_stream_t* stream, const uint8_t* data, size_t len);

/**
 * @brief End a stream (send half-close).
 */
void csilk_grpc_stream_end(csilk_grpc_stream_t* stream);

/**
 * @brief Check if a stream is closed.
 */
int csilk_grpc_stream_closed(const csilk_grpc_stream_t* stream);

/**
 * @brief Destroy a gRPC stream and release resources.
 */
void csilk_grpc_stream_destroy(csilk_grpc_stream_t* stream);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSILK_PROTOCOLS_GRPC_H */
