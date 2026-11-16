/**
 * @file http3_internal.h
 * @brief Internal HTTP/3 (QUIC) type definitions and transport abstraction.
 *
 * Defines the stream/connection/transport structs for the v1.2 HTTP/3
 * implementation. The QUIC transport vtable itself lives in
 * csilk/core/server/server.h (csilk_quic_transport_t).
 *
 * @copyright MIT License
 */

#ifndef CSILK_HTTP3_INTERNAL_H
#define CSILK_HTTP3_INTERNAL_H

#include "csilk/protocols/h3.h"

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CSILK_H3_MAX_STREAMS
#define CSILK_H3_MAX_STREAMS 128
#endif

#ifndef CSILK_H3_MAX_CONNS
#define CSILK_H3_MAX_CONNS 256
#endif

#ifdef CSILK_HAS_NGTCP2
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>
#include <openssl/ssl.h>
#endif

/** @brief Per-stream state for an HTTP/3 request/response pair. */
struct csilk_h3_stream_s {
    uint64_t stream_id;
    int      state; /* 0=idle, 1=open, 2=closed */
    void*    ctx;   /* csilk_ctx_t* (opaque) */
    char     method[16];
    char     path[512];
};

/** @brief Per-QUIC-connection state. */
struct csilk_h3_conn_s {
#ifdef CSILK_HAS_NGTCP2
    ngtcp2_conn*            ngtcp2_conn;
    ngtcp2_crypto_ossl_ctx* crypto_ctx;
    SSL*                    ssl;
    nghttp3_conn*           nghttp3_conn;
#endif
    struct csilk_h3_stream_s streams[CSILK_H3_MAX_STREAMS];
    int                      stream_count;
    void*                    client; /* csilk_client_t* (owner worker) */
    int                      is_server;
    uint64_t                 dcid;
};

/** @brief QUIC transport handle. */
struct csilk_h3_transport_s {
#ifdef CSILK_HAS_NGTCP2
    int      udp_fd;
    SSL_CTX* ssl_ctx;
    char     addr[64];
#endif
    struct csilk_h3_conn_s* conns[CSILK_H3_MAX_CONNS];
    int                     conn_count;
};

#ifdef __cplusplus
}
#endif

#endif /* CSILK_HTTP3_INTERNAL_H */
