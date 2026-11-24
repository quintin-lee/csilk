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
#include <ngtcp2/ngtcp2_crypto.h>
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
    ngtcp2_crypto_ossl_ctx* crypto_ctx;   /* per-conn TLS (OpenSSL QUIC adapter) */
    SSL*                    ssl;
    nghttp3_conn*           nghttp3_conn;
    ngtcp2_crypto_conn_ref  ssl_conn_ref; /* bridges SSL <-> ngtcp2_conn */
    /* Packet-protection AEAD + HKDF digest objects shared by the crypto
     * callbacks (encrypt/decrypt/hp_mask + key rotation). Native handles
     * point at the OpenSSL AES-GCM EVP_CIPHER / EVP_MD singletons. */
    ngtcp2_crypto_aead       aead;
    ngtcp2_crypto_cipher     hp; /* header-protection cipher (AES-GCM in v1) */
    ngtcp2_crypto_md         md; /* HKDF digest (SHA-256) */
    ngtcp2_crypto_aead_ctx   initial_tx_aead;
    ngtcp2_crypto_aead_ctx   initial_rx_aead;
    ngtcp2_crypto_cipher_ctx initial_tx_hp;
    ngtcp2_crypto_cipher_ctx initial_rx_hp;
    uint8_t                  initial_tx_iv[12];
    uint8_t                  initial_rx_iv[12];
    /* DCID bytes (max 20) + length; used by the ngtcp2 crypto callbacks
     * to derive packet-protection keys. Only meaningful under ngtcp2. */
    uint8_t dcid_data[NGTCP2_MAX_CIDLEN];
    size_t  dcid_len;
    uint8_t scid_data[NGTCP2_MAX_CIDLEN];
    size_t  scid_len;
#endif
    struct csilk_h3_stream_s     streams[CSILK_H3_MAX_STREAMS];
    int                          stream_count;
    void*                        client;    /* csilk_client_t* (owner worker) */
    struct csilk_h3_transport_s* transport; /* Owning transport */
    int                          is_server;
    /* Remote address for UDP reply. */
    struct sockaddr_storage remote;
    socklen_t               remote_len;
    /* Connection state: 0=new, 1=handshake, 2=established, 3=closed. */
    int state;
#ifdef CSILK_HAS_NGTCP2
    /* Per-connection scratch buffer for response NVs (replaces the
     * file-scope static in h3_session.c). */
    nghttp3_nv resp_nv[64];
    int        resp_nv_alloc;
#endif
};

/** @brief QUIC transport handle. */
struct csilk_h3_transport_s {
    int  udp_fd;
    int  udp_nonblocking;
    char addr[64];
#ifdef CSILK_HAS_NGTCP2
    SSL_CTX* ssl_ctx;
    /* Recv buffer (reused across recvfrom calls). */
    uint8_t recv_buf[65536];
#endif
    struct csilk_h3_conn_s* conns[CSILK_H3_MAX_CONNS];
    int                     conn_count;
    void*                   server; /* csilk_server_t* (owning server if any) */
    void*                   wp;     /* worker_pool_t* (owning worker pool if any) */
};

CSILK_INTERNAL int _h3_send_quic_packets(csilk_h3_transport_t* t, struct csilk_h3_conn_s* conn);
#ifdef CSILK_HAS_NGTCP2
CSILK_INTERNAL int                     _h3_flush_http3_to_quic(struct csilk_h3_conn_s* conn);
CSILK_INTERNAL struct csilk_h3_conn_s* _h3_new_conn(csilk_h3_transport_t*  t,
                                                    const ngtcp2_cid*      dcid,
                                                    const struct sockaddr* remote,
                                                    socklen_t              remote_len);
CSILK_INTERNAL void _h3_del_conn(csilk_h3_transport_t* t, struct csilk_h3_conn_s* c);
#endif
CSILK_INTERNAL int _h3_process_datagram(csilk_h3_transport_t*  t,
                                        const uint8_t*         buf,
                                        size_t                 len,
                                        const struct sockaddr* from,
                                        socklen_t              fromlen);
CSILK_INTERNAL csilk_h3_transport_t*
                    csilk_h3_worker_transport_new(csilk_server_t* server, void* wp, int udp_fd);
CSILK_INTERNAL void csilk_h3_worker_transport_destroy(csilk_h3_transport_t* t);
CSILK_INTERNAL void csilk_h3_conn_close(csilk_client_t* client);

/* HTTP/3 Streaming response & backpressure */
CSILK_INTERNAL int    csilk_h3_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len);
CSILK_INTERNAL void   csilk_h3_response_end(csilk_ctx_t* c);
CSILK_INTERNAL size_t csilk_h3_response_get_write_queue_size(csilk_ctx_t* c);
CSILK_INTERNAL int    csilk_h3_response_is_writable(csilk_ctx_t* c);

/* Stream mux tracking */
CSILK_INTERNAL struct csilk_h3_stream_s* _h3_get_or_create_stream(struct csilk_h3_conn_s* c,
                                                                  int64_t stream_id);
CSILK_INTERNAL struct csilk_h3_stream_s* _h3_get_stream(struct csilk_h3_conn_s* c,
                                                        int64_t                 stream_id);
CSILK_INTERNAL void _h3_remove_stream(struct csilk_h3_conn_s* c, int64_t stream_id);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_HTTP3_INTERNAL_H */
