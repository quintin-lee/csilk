/**
 * @file h3.h
 * @brief HTTP/3 (RFC 9114) Frame Encoding/Decoding, RFC 9000 QUIC Varint utilities,
 *        and full QUIC transport via ngtcp2/nghttp3.
 */

#ifndef CSILK_PROTOCOLS_H3_H
#define CSILK_PROTOCOLS_H3_H

#include "csilk/csilk.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations for OpenSSL QUIC API (Phase 1.3) */
struct ssl_st;
typedef struct ssl_st SSL;
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;

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

/* --- HTTP/3 QUIC Transport (v1.2+) --- */

/** @brief Opaque handle for HTTP/3 QUIC transport. */
struct csilk_h3_transport_s;
typedef struct csilk_h3_transport_s csilk_h3_transport_t;

/**
 * @brief Create a new HTTP/3 QUIC transport bound to a UDP port.
 *
 * @param port     UDP port to bind (e.g. 443).
 * @param cert_pem  PEM-encoded TLS certificate (NULL = no TLS, test only).
 * @param key_pem   PEM-encoded TLS private key.
 * @return Newly allocated transport handle, or NULL on failure.
 */
csilk_h3_transport_t* csilk_h3_transport_new(int port, const char* cert_pem, const char* key_pem);

/**
 * @brief Accept new QUIC connections on the transport.
 *
 * @param t      Transport handle.
 * @param out_conn [out] First newly-accepted connection, or NULL.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_transport_accept(csilk_h3_transport_t* t, void** out_conn);

/**
 * @brief Send data on an HTTP/3 stream.
 *
 * @param t          Transport handle.
 * @param conn       Connection handle.
 * @param stream_id  HTTP/3 stream ID.
 * @param data       Data to send.
 * @param len        Data length.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_transport_send(
    csilk_h3_transport_t* t, void* conn, uint64_t stream_id, const uint8_t* data, size_t len);

/**
 * @brief Receive data from an HTTP/3 stream.
 *
 * @param t        Transport handle.
 * @param conn     Connection handle.
 * @param stream_id  HTTP/3 stream ID.
 * @param buf      [out] Receive buffer.
 * @param buf_len  Buffer capacity.
 * @return Number of bytes received, or -1 on error.
 */
int csilk_h3_transport_recv(
    csilk_h3_transport_t* t, void* conn, uint64_t stream_id, uint8_t* buf, size_t buf_len);

/**
 * @brief Close an HTTP/3 stream.
 *
 * @param t          Transport handle.
 * @param conn       Connection handle.
 * @param stream_id  Stream ID to close.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_transport_close_stream(csilk_h3_transport_t* t, void* conn, uint64_t stream_id);

/**
 * @brief Close a QUIC connection.
 *
 * @param t     Transport handle.
 * @param conn  Connection handle.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_transport_close_conn(csilk_h3_transport_t* t, void* conn);

/**
 * @brief Destroy the transport and release all resources.
 *
 * @param t  Transport handle to free.
 */
void csilk_h3_transport_destroy(csilk_h3_transport_t* t);

/**
 * @brief Enable HTTP/3 on a csilk_server_t.
 *
 * @param server  Server instance.
 * @param port    UDP port for QUIC (e.g. 443).
 * @return 0 on success, -1 on failure.
 */
int csilk_h3_set_enabled(void* server, int port);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* --- HTTP/3 Stream data (always available) --- */

/** @brief Per-stream user data for HTTP/3 callbacks. */
typedef struct csilk_h3_stream_data_s {
    uint64_t       stream_id;
    char           method[16];
    char           path[512];
    uint32_t       num_headers;
    char**         header_names;
    char**         header_values;
    char*          body_buf;
    uint32_t       body_len;
    int            headers_complete;
    int            dispatched;
    const uint8_t* resp_body;
    size_t         resp_body_len;
    size_t         resp_body_offset;
    /* RFC 9114 Streams & Backpressure */
    csilk_ctx_t* stream_ctx;
    int          stream_ctx_owned;
    void*        h3_conn;           /**< Owning csilk_h3_conn_s* */
    size_t       write_queue_size;  /**< Unacknowledged outbound stream bytes */
    uint8_t*     stream_write_buf;  /**< Streaming response buffer */
    size_t       stream_write_len;  /**< Valid bytes in stream_write_buf */
    size_t       stream_write_cap;  /**< Allocated capacity of stream_write_buf */
    int          resp_headers_sent; /**< 1 if response headers submitted */
    int          resp_ended;        /**< 1 if csilk_response_end called */
} csilk_h3_stream_data_t;

/* --- HTTP/3 Session (nghttp3) — v1.2+ --- */

#ifdef CSILK_HAS_NGTCP2
#include <nghttp3/nghttp3.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for an HTTP/3 session (nghttp3_conn*). */
typedef struct nghttp3_conn csilk_h3_session_t;

/**
 * @brief Create an HTTP/3 session for a new QUIC connection.
 *
 * @param stream_id   First stream ID (typically 0 for server).
 * @param user_data   Per-connection user data.
 * @return Newly allocated session, or NULL on failure.
 */
csilk_h3_session_t* csilk_h3_session_new(int64_t stream_id, void* user_data);

/**
 * @brief Destroy an HTTP/3 session.
 */
void csilk_h3_session_destroy(csilk_h3_session_t* session);

/**
 * @brief Submit an HTTP/3 response on a stream.
 *
 * @param session    HTTP/3 session.
 * @param stream_id  Stream ID.
 * @param status     HTTP status code.
 * @param headers    Response headers (nghttp3_nv array).
 * @param nheaders   Number of headers.
 * @param body       Response body (may be NULL).
 * @param body_len   Body length.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_session_send_response(csilk_h3_session_t* session,
                                   int64_t             stream_id,
                                   int                 status,
                                   const nghttp3_nv*   headers,
                                   int                 nheaders,
                                   const uint8_t*      body,
                                   size_t              body_len);

/**
 * @brief Send HTTP/3 body data on a stream (for streaming responses).
 *
 * @param session    HTTP/3 session.
 * @param stream_id  Stream ID.
 * @param body       Body data.
 * @param body_len   Body length.
 * @return 0 on success, -1 on error.
 */
int csilk_h3_session_send_body(csilk_h3_session_t* session,
                               int64_t             stream_id,
                               const uint8_t*      body,
                               size_t              body_len);

/**
 * @brief Create a QUIC-enabled SSL object for ngtcp2's OpenSSL crypto adapter.
 *
 * Configures TLS 1.3 only (RFC 9001 mandates TLS 1.3 for QUIC).
 * The caller passes the resulting SSL* to ngtcp2_conn_server_new / _client_new
 * via the ngtcp2_crypto_ossl callback adapter.
 *
 * @param ssl_ctx  TLS context (from csilk_h3_transport_new).
 * @return A new SSL* configured for QUIC, or NULL on failure.
 */
SSL* csilk_h3_quic_tls_new(SSL_CTX* ssl_ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSILK_HAS_NGTCP2 */

/* --- HTTP/3 Stream data functions (always available) --- */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate per-stream user data for an HTTP/3 stream.
 */
csilk_h3_stream_data_t* csilk_h3_stream_data_new(uint64_t stream_id);

/**
 * @brief Free per-stream user data.
 */
void csilk_h3_stream_data_free(csilk_h3_stream_data_t* sd);

/**
 * @brief Dispatch a completed HTTP/3 stream into the shared router/middleware pipeline.
 *
 * Populates client->ctx from the collected stream data and invokes
 * _csilk_dispatch_request(), the same path used by HTTP/1.1 and HTTP/2.
 *
 * @param client  The owning csilk_client_t (from the per-worker pool).
 * @param sd      The completed stream data (headers_complete must be 1).
 * @return 0 on success, -1 if stream data is incomplete or client is NULL.
 */
int csilk_h3_dispatch_stream(csilk_client_t* client, csilk_h3_stream_data_t* sd);

/**
 * @brief Submit an HTTP/3 response for a completed request context.
 *
 * Encodes status and response headers via QPACK, writes body chunks into nghttp3,
 * and flushes resulting QUIC packets via UDP socket.
 *
 * @param ctx The request context.
 */
void csilk_h3_send_response_for_ctx(csilk_ctx_t* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSILK_PROTOCOLS_H3_H */
