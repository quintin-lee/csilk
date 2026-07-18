/**
 * @file h3.h
 * @brief HTTP/3 (RFC 9114) Frame Encoding/Decoding and RFC 9000 QUIC Varint utilities.
 */

#ifndef CSILK_PROTOCOLS_H3_H
#define CSILK_PROTOCOLS_H3_H

#include "csilk/csilk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP/3 Frame Types (RFC 9114) */
#define CSILK_H3_FRAME_DATA 0x00ULL
#define CSILK_H3_FRAME_HEADERS 0x01ULL
#define CSILK_H3_FRAME_CANCEL_PUSH 0x02ULL
#define CSILK_H3_FRAME_SETTINGS 0x04ULL
#define CSILK_H3_FRAME_PUSH_PROMISE 0x05ULL
#define CSILK_H3_FRAME_GOAWAY 0x07ULL
#define CSILK_H3_FRAME_MAX_PUSH_ID 0x0dULL

/**
 * @brief Encode a 64-bit integer into RFC 9000 QUIC Variable-Length Integer (Varint) format.
 *
 * @param val Integer value to encode (must be <= 4611686018427387903ULL).
 * @param[out] out_buf Destination buffer (must be at least 8 bytes).
 * @return Number of encoded bytes (1, 2, 4, or 8), or 0 on error.
 */
size_t csilk_h3_varint_encode(uint64_t val, uint8_t* out_buf);

/**
 * @brief Decode an RFC 9000 QUIC Variable-Length Integer from input buffer.
 *
 * @param in_buf Input buffer containing encoded bytes.
 * @param len Byte length of input buffer.
 * @param[out] out_val Pointer to store decoded 64-bit integer.
 * @return Number of bytes read from input buffer (1, 2, 4, or 8), or 0 if buffer is truncated/invalid.
 */
size_t csilk_h3_varint_decode(const uint8_t* in_buf, size_t len, uint64_t* out_val);

/**
 * @brief Serialize an HTTP/3 frame (Type + Length + Payload) into destination buffer.
 *
 * @param frame_type HTTP/3 frame type identifier (e.g. CSILK_H3_FRAME_DATA).
 * @param payload Raw payload byte array.
 * @param payload_len Length of @p payload in bytes.
 * @param[out] out_buf Destination frame buffer.
 * @param max_buf_len Capacity of @p out_buf in bytes.
 * @return Total serialized HTTP/3 frame byte length, or 0 on failure.
 */
size_t csilk_h3_frame_encode(uint64_t       frame_type,
                             const uint8_t* payload,
                             size_t         payload_len,
                             uint8_t*       out_buf,
                             size_t         max_buf_len);

/**
 * @brief Decode an HTTP/3 frame header from input buffer.
 *
 * @param in_buf Input frame buffer.
 * @param in_len Byte length of @p in_buf.
 * @param[out] out_type Stores decoded frame type.
 * @param[out] out_payload_offset Stores byte offset where frame payload starts.
 * @param[out] out_payload_len Stores decoded payload byte length.
 * @return 0 on success, or -1 if buffer is incomplete/corrupted.
 */
int csilk_h3_frame_decode(const uint8_t* in_buf,
                          size_t         in_len,
                          uint64_t*      out_type,
                          size_t*        out_payload_offset,
                          size_t*        out_payload_len);

/**
 * @brief Inject HTTP/3 Alt-Svc header into response context.
 *
 * @param ctx Request context.
 * @param port UDP port providing HTTP/3 QUIC service (e.g. 443).
 */
void csilk_h3_inject_alt_svc_header(csilk_ctx_t* ctx, int port);

/* --- HTTP/3 UDP Socket Listener --- */

/** @brief Opaque handle for HTTP/3 UDP Socket Listener. */
typedef struct csilk_h3_listener_s csilk_h3_listener_t;

/**
 * @brief Bind an HTTP/3 UDP Socket Listener to a local port.
 *
 * @param port UDP port number to bind (e.g. 443).
 * @return Heap-allocated listener handle, or NULL on bind error.
 */
csilk_h3_listener_t* csilk_h3_listener_bind(int port);

/**
 * @brief Process an incoming QUIC/HTTP/3 packet and extract the 64-bit Connection ID.
 *
 * @param listener Active HTTP/3 UDP listener handle.
 * @param packet Raw UDP packet byte buffer.
 * @param len Length of UDP packet in bytes.
 * @param[out] out_conn_id Stores extracted 64-bit Connection ID.
 * @return 0 on successful packet parsing, -1 on invalid/corrupted packet.
 */
int csilk_h3_listener_process_packet(csilk_h3_listener_t* listener,
                                     const uint8_t*       packet,
                                     size_t               len,
                                     uint64_t*            out_conn_id);

/**
 * @brief Close and free an HTTP/3 UDP Socket Listener.
 *
 * @param listener Listener handle to release.
 */
void csilk_h3_listener_close(csilk_h3_listener_t* listener);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_PROTOCOLS_H3_H */
