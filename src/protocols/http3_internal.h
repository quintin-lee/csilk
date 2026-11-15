/**
 * @file http3_internal.h
 * @brief Internal HTTP/3 (QUIC) type definitions and transport abstraction.
 *
 * This header defines the internal structures and transport vtable for the
 * HTTP/3 QUIC layer. The actual QUIC handshake, stream management, and
 * nghttp3 integration are implemented in src/protocols/http3.c (v1.2+).
 *
 * In v1.1, these types serve as architectural groundwork: the csilk_quic_transport_t
 * interface is defined and registered on the server, but the default implementation
 * is a fail-closed stub that returns -ENOTSUP.
 *
 * @copyright MIT License
 */

#ifndef CSILK_HTTP3_INTERNAL_H
#define CSILK_HTTP3_INTERNAL_H

#include "csilk/core/server/server.h"
#include "csilk/protocols/http3.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief QUIC transport vtable — one implementation per event-loop backend. */
typedef struct {
    /** @brief Initialize the QUIC transport on the event loop.
     *  @param loop   Event loop handle.
     *  @param addr   Bind address (e.g. "0.0.0.0:443").
     *  @param cert   PEM certificate data (NULL = no TLS).
     *  @param key    PEM private key data.
     *  @return 0 on success, -1 on failure. */
    int (*init)(void* loop, const char* addr, const char* cert, const char* key);

    /** @brief Accept a new QUIC connection.
     *  @param transport  Transport state.
     *  @param out_conn   [out] Accepted connection handle.
     *  @return 0 on success, -1 if no connections pending. */
    int (*accept)(void* transport, void** out_conn);

    /** @brief Send HTTP/3 frame data on a QUIC stream.
     *  @param transport  Transport state.
     *  @param conn       Connection handle.
     *  @param stream_id  Stream ID.
     *  @param data       Data to send.
     *  @param len        Data length.
     *  @return 0 on success, -1 on error. */
    int (*send)(void* transport, void* conn, uint64_t stream_id, const uint8_t* data, size_t len);

    /** @brief Receive HTTP/3 frame data from a QUIC stream.
     *  @param transport  Transport state.
     *  @param conn       Connection handle.
     *  @param stream_id  Stream ID.
     *  @param buf        [out] Receive buffer.
     *  @param buf_len    Buffer capacity.
     *  @return Bytes received, -1 on error. */
    int (*recv)(void* transport, void* conn, uint64_t stream_id, uint8_t* buf, size_t buf_len);

    /** @brief Close a QUIC stream.
     *  @param transport  Transport state.
     *  @param conn       Connection handle.
     *  @param stream_id  Stream ID.
     *  @return 0 on success, -1 on error. */
    int (*close_stream)(void* transport, void* conn, uint64_t stream_id);

    /** @brief Close a QUIC connection.
     *  @param transport  Transport state.
     *  @param conn       Connection handle.
     *  @return 0 on success, -1 on error. */
    int (*close_conn)(void* transport, void* conn);

    /** @brief Destroy the transport and release all resources.
     *  @param transport  Transport state to free. */
    void (*destroy)(void* transport);
} csilk_quic_transport_t;

/*
 * NOTE: ngtcp2/nghttp3 types are intentionally NOT included here.
 * The v1.1 header defines only the transport vtable (architectural groundwork).
 * Concrete ngtcp2/nghttp3 integration types and state machines are added
 * in the v1.2 implementation file (src/protocols/http3.c).
 */

#ifdef __cplusplus
}
#endif

#endif /* CSILK_HTTP3_INTERNAL_H */
