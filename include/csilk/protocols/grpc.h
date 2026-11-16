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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSILK_PROTOCOLS_GRPC_H */
