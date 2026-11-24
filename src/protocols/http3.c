/**
 * @file http3.c
 * @brief HTTP/3 (RFC 9114) QUIC transport implementation via ngtcp2 + nghttp3.
 *
 * Implements the csilk_quic_transport_t vtable (from server.h) against
 * system ngtcp2 and nghttp3 libraries. Guarded by CSILK_HAS_NGTCP2 so the
 * file compiles cleanly even when ngtcp2 is unavailable (fail-closed).
 *
 * The OpenSSL QUIC crypto adapter (ngtcp2_crypto_ossl) drives the TLS 1.3
 * handshake through OpenSSL's SSL_set_quic_tls_cbs(). The ngtcp2 crypto
 * callbacks (encrypt/decrypt/hp_mask + key rotation) are supplied by the
 * adapter's public vtable (ngtcp2_crypto_aead_ossl, ngtcp2_cipher_aead_aes_gcm,
 * ngtcp2_cipher_cipher_aes_gcm, ngtcp2_ossl_hkdf). A per-connection
 * ngtcp2_crypto_ossl_ctx bridges the SSL object and the ngtcp2_conn via
 * ngtcp2_crypto_conn_ref (SSL_set_app_data), so the adapter can read/write
 * handshake data.
 *
 * @copyright MIT License
 */

#include "csilk/protocols/h3.h"
#include "csilk/csilk.h"
#include "http3_internal.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CSILK_HAS_NGTCP2
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#endif /* CSILK_HAS_NGTCP2 */

#ifdef CSILK_HAS_NGTCP2

/* Forward-declare the TLS setup helper (defined later in this file). */
static int _h3_init_ssl_ctx(csilk_h3_transport_t* t, const char* cert_pem, const char* key_pem);

/* ===================================================================
 * QUIC crypto callbacks
 *
 * The OpenSSL QUIC crypto adapter (libngtcp2_crypto_ossl) drives the
 * TLS 1.3 handshake through SSL_set_quic_tls_cbs().  The generic
 * ngtcp2_crypto_* wrappers (from ngtcp2_crypto.h, implemented by
 * libngtcp2_crypto_ossl.so) are passed directly into ngtcp2_callbacks:
 *   - ngtcp2_crypto_encrypt_cb / _decrypt_cb / _hp_mask_cb
 *   - ngtcp2_crypto_update_key_cb
 *   - ngtcp2_crypto_recv_crypto_data_cb
 *   - ngtcp2_crypto_recv_client_initial_cb
 *   - ngtcp2_crypto_delete_crypto_aead_ctx_cb / _cipher_ctx_cb
 *   - ngtcp2_crypto_get_path_challenge_data2_cb
 *
 * The conn_ref pattern: SSL_set_app_data(ssl, &conn_ref) where
 * conn_ref.get_conn returns the ngtcp2_conn*.
 * =================================================================== */

/* conn_ref.get_conn callback: return the ngtcp2_conn from the h3 conn. */
static ngtcp2_conn*
_h3_get_conn(ngtcp2_crypto_conn_ref* conn_ref)
{
    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)conn_ref->user_data;
    return c ? c->ngtcp2_conn : NULL;
}

/* QUIC recv_client_initial callback: set up the SSL + crypto adapter for a
 * new server connection, and install the Initial-packet protection keys
 * derived from the DCID.  Uses ngtcp2_crypto_recv_client_initial_cb which
 * internally calls ngtcp2_crypto_ossl_configure_server_session +
 * ngtcp2_conn_install_initial_key. */
static int
_h3_recv_client_initial(ngtcp2_conn* conn, const ngtcp2_cid* dcid, void* user_data)
{
    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)user_data;
    if (!c) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    /* Configure the SSL object for QUIC server side (sets up the QUIC TLS
     * callbacks via SSL_set_quic_tls_cbs). */
    if (!c->ssl) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (ngtcp2_crypto_ossl_configure_server_session(c->ssl) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    /* Bridge the SSL <-> ngtcp2_conn via conn_ref (SSL_set_app_data).
     * The OpenSSL adapter reads/writes handshake data through this
     * reference.  Store the conn_ref on the conn so it outlives the
     * stack. */
    c->ssl_conn_ref.get_conn = _h3_get_conn;
    c->ssl_conn_ref.user_data = c;
    SSL_set_app_data(c->ssl, &c->ssl_conn_ref);

    /* Install the Initial-packet AEAD + header-protection keys derived
     * from the DCID, then set the TLS native handle. */
    int rc = ngtcp2_crypto_recv_client_initial_cb(conn, dcid, c);
    if (rc != 0) {
        CSILK_LOG_W("H3: recv_client_initial failed: %d", rc);
        return rc;
    }

    /* Point ngtcp2 at the conn_ref so the OpenSSL adapter can retrieve
     * the ngtcp2_conn* during the handshake. */
    ngtcp2_conn_set_tls_native_handle(conn, &c->ssl_conn_ref);
    return 0;
}

/* QUIC handshake_completed callback: mark the connection established. */
static int
_h3_handshake_completed(ngtcp2_conn* conn, void* user_data)
{
    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)user_data;
    if (c) {
        c->state = 2; /* established */
    }
    /* Notify ngtcp2 that the TLS handshake has completed so it can
     * switch to 1-RTT keys. */
    ngtcp2_conn_tls_handshake_completed(conn);
    return 0;
}

/* QUIC get_path_challenge_data2 callback: generate PATH_CHALLENGE data.
 * Delegates to the generic ngtcp2_crypto wrapper. */
static int
_h3_get_path_challenge_data2(ngtcp2_conn* conn, ngtcp2_path_challenge_data* data, void* user_data)
{
    return ngtcp2_crypto_get_path_challenge_data2_cb(conn, data, user_data);
}

/* QUIC get_new_connection_id2 callback: mint a new CID + reset token. */
static int
_h3_get_new_connection_id2(ngtcp2_conn*                  conn,
                           ngtcp2_cid*                   cid,
                           ngtcp2_stateless_reset_token* token,
                           size_t                        cidlen,
                           void*                         user_data)
{
    (void)conn;
    (void)user_data;
    if (cidlen > NGTCP2_MAX_CIDLEN) {
        cidlen = NGTCP2_MAX_CIDLEN;
    }
    cid->datalen = cidlen;
    RAND_bytes(cid->data, (int)cidlen);
    RAND_bytes(token->data, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

/* QUIC rand callback: OpenSSL RAND_bytes. */
static void
_h3_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx)
{
    (void)rand_ctx;
    RAND_bytes(dest, (int)destlen);
}

/* QUIC tls_early_data_rejected callback: 0-RTT early data was rejected by peer. */
static int
_h3_tls_early_data_rejected(ngtcp2_conn* conn, void* user_data)
{
    (void)conn;
    (void)user_data;
    CSILK_LOG_W("H3: 0-RTT early data rejected by TLS peer");
    return 0;
}

/* QUIC recv_stream_data callback: forward data to nghttp3. */
static int
_h3_recv_stream_data(ngtcp2_conn*   conn,
                     uint32_t       flags,
                     int64_t        stream_id,
                     uint64_t       offset,
                     const uint8_t* data,
                     size_t         datalen,
                     void*          user_data,
                     void*          stream_user_data)
{
    (void)offset;
    (void)stream_user_data;
    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)user_data;
    if (!c || !c->nghttp3_conn) {
        return 0;
    }

    int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0;

    /* Feed the raw HTTP/3 stream payload into nghttp3. */
    nghttp3_ssize consumed =
        nghttp3_conn_read_stream(c->nghttp3_conn, stream_id, data, datalen, fin);
    if (consumed < 0) {
        CSILK_LOG_W("H3: nghttp3 read_stream error on stream %" PRId64 ": %s",
                    stream_id,
                    nghttp3_strerror((int)consumed));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (consumed > 0) {
        /* Inform nghttp3 of the write offset we consumed. */
        nghttp3_conn_add_write_offset(c->nghttp3_conn, stream_id, (size_t)consumed);

        /* Update QUIC flow control windows */
        ngtcp2_conn_extend_max_stream_offset(conn, stream_id, (uint64_t)consumed);
        ngtcp2_conn_extend_max_offset(conn, (uint64_t)consumed);
    }
    return 0;
}

/* Delete callbacks: free per-conn crypto ctxs via the generic ngtcp2_crypto
 * wrappers (libngtcp2_crypto_ossl.so provides the native_handle cleanup). */
static void
_h3_delete_aead_ctx(ngtcp2_conn* conn, ngtcp2_crypto_aead_ctx* aead_ctx, void* user_data)
{
    (void)user_data;
    ngtcp2_crypto_aead_ctx_free(aead_ctx);
}

static void
_h3_delete_cipher_ctx(ngtcp2_conn* conn, ngtcp2_crypto_cipher_ctx* cipher_ctx, void* user_data)
{
    (void)conn;
    (void)user_data;
    /* The generic ngtcp2_crypto_delete_crypto_cipher_ctx_cb frees the
     * native_handle (EVP_CIPHER_CTX) via the OpenSSL adapter. */
    ngtcp2_crypto_delete_crypto_cipher_ctx_cb(conn, cipher_ctx, user_data);
}

/**
 * @brief Build the ngtcp2_callbacks struct for this H3 engine.
 *
 * Uses the generic ngtcp2_crypto_* wrappers (implemented by
 * libngtcp2_crypto_ossl.so) for the crypto callbacks and the app-side
 * callbacks for everything else.
 */
static ngtcp2_callbacks
_h3_callbacks(void)
{
    ngtcp2_callbacks cbs = {0};
    cbs.recv_client_initial = _h3_recv_client_initial;
    cbs.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cbs.handshake_completed = _h3_handshake_completed;
    cbs.recv_stream_data = _h3_recv_stream_data;
    cbs.update_key = ngtcp2_crypto_update_key_cb;
    cbs.rand = _h3_rand;
    cbs.get_new_connection_id2 = _h3_get_new_connection_id2;
    cbs.get_path_challenge_data2 = _h3_get_path_challenge_data2;
    cbs.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cbs.delete_crypto_cipher_ctx = _h3_delete_cipher_ctx;
    cbs.encrypt = ngtcp2_crypto_encrypt_cb;
    cbs.decrypt = ngtcp2_crypto_decrypt_cb;
    cbs.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cbs.tls_early_data_rejected = _h3_tls_early_data_rejected;
    /* version_negotiation is required by NGTCP2_CALLBACKS_V3+; the
     * generic wrapper installs Initial keys for the negotiated version. */
    cbs.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    return cbs;
}

/* ===================================================================
 * Connection management
 * =================================================================== */

/**
 * @brief Look up a QUIC connection by DCID.
 *
 * @param t      Transport handle.
 * @param dcid   Destination connection ID.
 * @return The matching csilk_h3_conn_s*, or NULL if not found.
 */
static struct csilk_h3_conn_s*
_h3_find_conn(csilk_h3_transport_t* t, const uint8_t* dcid, size_t dcid_len)
{
    if (!dcid || dcid_len == 0) {
        return NULL;
    }
    for (int i = 0; i < t->conn_count; i++) {
        struct csilk_h3_conn_s* c = t->conns[i];
        if (!c || c->state == 3) {
            continue;
        }
        /* Match against server SCID (1-RTT/Handshake packets) or Initial DCID. */
        if (c->scid_len > 0 && dcid_len >= c->scid_len &&
            memcmp(dcid, c->scid_data, c->scid_len) == 0) {
            return c;
        }
        size_t matchlen = dcid_len < c->dcid_len ? dcid_len : c->dcid_len;
        if (matchlen > 0 && memcmp(dcid, c->dcid_data, matchlen) == 0) {
            return c;
        }
    }
    return NULL;
}

/**
 * @brief Allocate and register a new QUIC connection.
 */
CSILK_INTERNAL struct csilk_h3_conn_s*
_h3_new_conn(csilk_h3_transport_t*  t,
             const ngtcp2_cid*      dcid,
             const struct sockaddr* remote,
             socklen_t              remote_len)
{
    if (t->conn_count >= CSILK_H3_MAX_CONNS) {
        CSILK_LOG_W("H3: max connections (%d) reached, dropping new conn", CSILK_H3_MAX_CONNS);
        return NULL;
    }

    struct csilk_h3_conn_s* c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }

    c->is_server = 1;
    c->state = 0; /* new */
    c->stream_count = 0;
    c->transport = t;

    worker_pool_t*  wp = (worker_pool_t*)t->wp;
    csilk_server_t* srv = (csilk_server_t*)t->server;
    if (wp) {
        c->client = pool_get(wp);
        if (c->client) {
            csilk_client_t* cl = (csilk_client_t*)c->client;
            cl->protocol = CSILK_PROTO_HTTP3;
            cl->h3_conn = c;
            cl->server = srv;
            cl->owner_pool = wp;
            cl->ctx._internal_client = cl;
            cl->ctx.server = srv;
            cl->ctx.arena = pool_get_arena(wp);
            cl->ctx.file_fd = -1;
            cl->ctx.handler_index = -1;
            cl->ctx.request_seq = 1;
            cl->ctx.write_high_water_mark = CSILK_WRITE_HWM_DEFAULT;
            cl->ctx.write_low_water_mark = CSILK_WRITE_LWM_DEFAULT;
            cl->ctx.max_write_buffer_size = CSILK_WRITE_MAX_BUFFER_DEFAULT;
            csilk_conn_set_state(cl, CSILK_CONN_ACCEPTED);
            if (srv) {
                client_list_add(srv, cl);
            }
        }
    } else {
        c->client = calloc(1, sizeof(csilk_client_t));
        if (c->client) {
            csilk_client_t* cl = (csilk_client_t*)c->client;
            cl->protocol = CSILK_PROTO_HTTP3;
            cl->h3_conn = c;
            cl->server = srv;
            cl->ctx._internal_client = cl;
            cl->ctx.server = srv;
            cl->ctx.arena = csilk_arena_new(4096);
            cl->ctx.file_fd = -1;
            cl->ctx.handler_index = -1;
            cl->ctx.request_seq = 1;
            cl->ctx.write_high_water_mark = CSILK_WRITE_HWM_DEFAULT;
            cl->ctx.write_low_water_mark = CSILK_WRITE_LWM_DEFAULT;
            cl->ctx.max_write_buffer_size = CSILK_WRITE_MAX_BUFFER_DEFAULT;
            csilk_conn_set_state(cl, CSILK_CONN_ACCEPTED);
        }
    }
    c->dcid_len = dcid->datalen;
    if (dcid->datalen > 0) {
        memcpy(c->dcid_data,
               dcid->data,
               dcid->datalen > sizeof(c->dcid_data) ? sizeof(c->dcid_data) : dcid->datalen);
    }
    c->remote = *(const struct sockaddr_storage*)remote;
    c->remote_len = remote_len;

    t->conns[t->conn_count] = c;
    t->conn_count++;
    return c;
}

/* Forward. */
CSILK_INTERNAL int _h3_send_quic_packets(csilk_h3_transport_t* t, struct csilk_h3_conn_s* conn);

/**
 * @brief Unregister and free a QUIC connection.
 */
CSILK_INTERNAL void
_h3_del_conn(csilk_h3_transport_t* t, struct csilk_h3_conn_s* c)
{
    if (!c) {
        return;
    }

    if (c->nghttp3_conn) {
        nghttp3_conn_del(c->nghttp3_conn);
        c->nghttp3_conn = NULL;
    }
    if (c->ngtcp2_conn) {
        ngtcp2_conn_del(c->ngtcp2_conn);
        c->ngtcp2_conn = NULL;
    }
    /* Release the per-connection crypto ctx (frees SSL QUIC state). */
    ngtcp2_crypto_ossl_ctx_del(c->crypto_ctx);
    c->crypto_ctx = NULL;
    if (c->ssl) {
        /* NULL the conn_ref before freeing SSL (per ngtcp2_crypto_ossl docs) */
        SSL_set_app_data(c->ssl, NULL);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }

    worker_pool_t*  wp = (worker_pool_t*)t->wp;
    csilk_server_t* srv = (csilk_server_t*)t->server;

    if (c->client) {
        csilk_client_t* cl = (csilk_client_t*)c->client;
        csilk_conn_set_state(cl, CSILK_CONN_CLOSING);
        csilk_conn_set_state(cl, CSILK_CONN_CLOSED);
        if (wp) {
            if (srv) {
                client_list_remove(srv, cl);
            }
            if (cl->ctx.arena) {
                pool_put_arena(wp, cl->ctx.arena);
                cl->ctx.arena = NULL;
            }
            csilk_ctx_cleanup(&cl->ctx);
            pool_put(wp, cl);
        } else {
            if (cl->ctx.arena) {
                csilk_arena_free(cl->ctx.arena);
                cl->ctx.arena = NULL;
            }
            csilk_ctx_cleanup(&cl->ctx);
            free(cl);
        }
        c->client = NULL;
    }

    /* Remove from transport's connection table. */
    for (int i = 0; i < t->conn_count; i++) {
        if (t->conns[i] == c) {
            t->conns[i] = t->conns[--t->conn_count];
            break;
        }
    }
    free(c);
}

/* ===================================================================
 * QUIC packet I/O
 * =================================================================== */

/**
 * @brief Build an ngtcp2_path from a sockaddr_storage.
 */
static ngtcp2_path_storage
_h3_make_path(const struct sockaddr_storage* remote,
              socklen_t                      remote_len,
              const struct sockaddr*         local,
              socklen_t                      local_len)
{
    ngtcp2_path_storage ps;
    memset(&ps, 0, sizeof(ps));

    if (remote && remote_len > 0) {
        memcpy(&ps.remote_addrbuf, remote, remote_len);
        ps.path.remote.addr = (ngtcp2_sockaddr*)&ps.remote_addrbuf;
        ps.path.remote.addrlen = remote_len;
    }
    if (local && local_len > 0) {
        memcpy(&ps.local_addrbuf, local, local_len);
        ps.path.local.addr = (ngtcp2_sockaddr*)&ps.local_addrbuf;
        ps.path.local.addrlen = local_len;
    }
    return ps;
}

/**
 * @brief Send all pending QUIC packets for a connection.
 *
 * @param t     Transport handle.
 * @param conn  The QUIC connection.
 * @return 0 on success, -1 on error.
 */
CSILK_INTERNAL int
_h3_send_quic_packets(csilk_h3_transport_t* t, struct csilk_h3_conn_s* conn)
{
    uint8_t pktbuf[NGTCP2_MAX_UDP_PAYLOAD_SIZE + 64];

    struct sockaddr_storage local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    socklen_t local_len = sizeof(local_addr);
    getsockname(t->udp_fd, (struct sockaddr*)&local_addr, &local_len);

    ngtcp2_path_storage ps = _h3_make_path((const struct sockaddr_storage*)&conn->remote,
                                           conn->remote_len,
                                           (const struct sockaddr*)&local_addr,
                                           local_len);

    ngtcp2_tstamp ts = csilk_io_hrtime();

    for (int i = 0; i < 8; i++) {
        ngtcp2_pkt_info pi;
        memset(&pi, 0, sizeof(pi));

        ngtcp2_ssize wlen = ngtcp2_conn_write_pkt_versioned(
            conn->ngtcp2_conn, &ps.path, NGTCP2_PKT_INFO_VERSION, &pi, pktbuf, sizeof(pktbuf), ts);
        if (wlen <= 0) {
            break; /* no more packets (or terminal state) */
        }

        /* Send via UDP */
        struct sockaddr* dest = (struct sockaddr*)&conn->remote;
        ssize_t          n = sendto(t->udp_fd, pktbuf, (size_t)wlen, 0, dest, conn->remote_len);
        if (n > 0) {
            ngtcp2_conn_update_pkt_tx_time(conn->ngtcp2_conn, ts);
        } else if (n < 0 && errno != EWOULDBLOCK) {
            CSILK_LOG_W("H3: sendto failed: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Flush outbound HTTP/3 data (control frames, QPACK, responses) into QUIC streams.
 *
 * @param conn  HTTP/3 connection.
 * @return 0 on success, -1 on error.
 */
CSILK_INTERNAL int
_h3_flush_http3_to_quic(struct csilk_h3_conn_s* conn)
{
    if (!conn || !conn->nghttp3_conn || !conn->ngtcp2_conn) {
        return 0;
    }

    while (1) {
        int64_t       stream_id = -1;
        int           fin = 0;
        nghttp3_vec   vec[16];
        nghttp3_ssize nvec =
            nghttp3_conn_writev_stream(conn->nghttp3_conn, &stream_id, &fin, vec, 16);
        if (nvec < 0) {
            CSILK_LOG_E("H3: nghttp3_conn_writev_stream error: %s", nghttp3_strerror((int)nvec));
            return -1;
        }
        if (nvec == 0) {
            if (fin && stream_id >= 0) {
                uint8_t      pktbuf[NGTCP2_MAX_UDP_PAYLOAD_SIZE + 64];
                ngtcp2_ssize written = ngtcp2_conn_writev_stream(conn->ngtcp2_conn,
                                                                 NULL,
                                                                 NULL,
                                                                 pktbuf,
                                                                 sizeof(pktbuf),
                                                                 NULL,
                                                                 NGTCP2_WRITE_STREAM_FLAG_FIN,
                                                                 stream_id,
                                                                 NULL,
                                                                 0,
                                                                 csilk_io_hrtime());
                (void)written;
                nghttp3_conn_add_write_offset(conn->nghttp3_conn, stream_id, 0);
            }
            break;
        }

        ngtcp2_vec qvec[16];
        for (int i = 0; i < nvec; i++) {
            qvec[i].base = vec[i].base;
            qvec[i].len = vec[i].len;
        }

        uint8_t      pktbuf[NGTCP2_MAX_UDP_PAYLOAD_SIZE + 64];
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize written = ngtcp2_conn_writev_stream(conn->ngtcp2_conn,
                                                         NULL,
                                                         NULL,
                                                         pktbuf,
                                                         sizeof(pktbuf),
                                                         &pdatalen,
                                                         fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0,
                                                         stream_id,
                                                         qvec,
                                                         (size_t)nvec,
                                                         csilk_io_hrtime());
        if (written < 0) {
            if (written == NGTCP2_ERR_STREAM_NOT_FOUND) {
                nghttp3_conn_close_stream(
                    conn->nghttp3_conn, stream_id, NGHTTP3_H3_STREAM_CREATION_ERROR);
            }
            break;
        }

        if (pdatalen > 0) {
            nghttp3_conn_add_write_offset(conn->nghttp3_conn, stream_id, (size_t)pdatalen);
        }
    }
    return 0;
}

/**
 * @brief Process a single received QUIC datagram.
 *
 * @param t       Transport handle.
 * @param buf     Datagram bytes.
 * @param len     Datagram length.
 * @param from    Sender address.
 * @param fromlen Sender address length.
 * @return 0 on success, -1 on error.
 */
static int
_h3_process_datagram(csilk_h3_transport_t*  t,
                     const uint8_t*         buf,
                     size_t                 len,
                     const struct sockaddr* from,
                     socklen_t              fromlen)
{
    if (len < 16) {
        return 0; /* too short to be a QUIC packet */
    }

    /* Parse the QUIC header to extract the DCID.
     *
     * The DCID is extracted from the packet via ngtcp2_accept() which
     * validates the first packet of a connection and decodes the header
     * (including DCID/SCID). For established connections, the DCID is
     * read from the header directly. */
    struct csilk_h3_conn_s* conn = NULL;
    ngtcp2_version_cid      vc;
    if (ngtcp2_pkt_decode_version_cid(&vc, buf, len, 8) == 0 && vc.dcid && vc.dcidlen > 0) {
        conn = _h3_find_conn(t, vc.dcid, vc.dcidlen);
    }

    ngtcp2_pkt_hd hd;
    if (!conn) {
        int rc = ngtcp2_accept(&hd, buf, len);
        if (rc != 0 && rc != NGTCP2_ERR_VERSION_NEGOTIATION) {
            /* Not an acceptable first packet and not an active conn; drop it. */
            return 0;
        }
        conn = _h3_find_conn(t, hd.dcid.data, hd.dcid.datalen);
    }

    if (!conn) {
        /* New connection: create it + ngtcp2 + crypto + nghttp3 session */
        conn = _h3_new_conn(t, &hd.dcid, from, fromlen);
        if (!conn) {
            return 0;
        }

        /* Initialize the per-connection SSL object for QUIC. */
        conn->ssl = csilk_h3_quic_tls_new(t->ssl_ctx);
        if (!conn->ssl) {
            _h3_del_conn(t, conn);
            return -1;
        }

        /* Initialize the ngtcp2 crypto context (bridges SSL + QUIC). */
        int rc = ngtcp2_crypto_ossl_ctx_new(&conn->crypto_ctx, conn->ssl);
        if (rc != 0) {
            SSL_free(conn->ssl);
            conn->ssl = NULL;
            _h3_del_conn(t, conn);
            return -1;
        }

        struct sockaddr_storage local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        socklen_t local_len = sizeof(local_addr);
        getsockname(t->udp_fd, (struct sockaddr*)&local_addr, &local_len);

        ngtcp2_path_storage ps = _h3_make_path((const struct sockaddr_storage*)from,
                                               fromlen,
                                               (const struct sockaddr*)&local_addr,
                                               local_len);

        /* Generate a server connection ID. */
        ngtcp2_cid scid;
        scid.datalen = 8;
        RAND_bytes(scid.data, scid.datalen);
        conn->scid_len = scid.datalen;
        memcpy(conn->scid_data, scid.data, scid.datalen);

        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_rtt = NGTCP2_DEFAULT_INITIAL_RTT;

        ngtcp2_transport_params tp;
        ngtcp2_transport_params_default(&tp);
        tp.initial_max_streams_bidi = 128;
        tp.initial_max_streams_uni = 128;
        tp.initial_max_stream_data_bidi_local = 256 * 1024;
        tp.initial_max_stream_data_bidi_remote = 256 * 1024;
        tp.initial_max_stream_data_uni = 256 * 1024;
        tp.initial_max_data = 1024 * 1024;
        tp.max_idle_timeout = 30 * NGTCP2_SECONDS;
        tp.original_dcid = hd.dcid;
        tp.original_dcid_present = 1;
        tp.initial_scid = scid;
        tp.initial_scid_present = 1;
        tp.active_connection_id_limit = 4;

        ngtcp2_callbacks cbs = _h3_callbacks();

        rc = ngtcp2_conn_server_new_versioned(&conn->ngtcp2_conn,
                                              &hd.dcid,
                                              &scid,
                                              &ps.path,
                                              NGTCP2_PROTO_VER_V1,
                                              NGTCP2_CALLBACKS_VERSION,
                                              &cbs,
                                              NGTCP2_SETTINGS_VERSION,
                                              &settings,
                                              NGTCP2_TRANSPORT_PARAMS_VERSION,
                                              &tp,
                                              NULL,  /* mem */
                                              conn); /* user_data = the h3 conn */
        if (rc != 0) {
            CSILK_LOG_W("H3: ngtcp2_conn_server_new failed: %s", ngtcp2_strerror(rc));
            ngtcp2_crypto_ossl_ctx_del(conn->crypto_ctx);
            conn->crypto_ctx = NULL;
            SSL_free(conn->ssl);
            conn->ssl = NULL;
            _h3_del_conn(t, conn);
            return -1;
        }

        /* Set the conn_ref as TLS native handle so the OpenSSL adapter
         * can find the ngtcp2_conn* via SSL_get_app_data. */
        ngtcp2_conn_set_tls_native_handle(conn->ngtcp2_conn, &conn->ssl_conn_ref);

        /* Create the nghttp3 session. */
        conn->nghttp3_conn = csilk_h3_session_new(0, conn);
        if (!conn->nghttp3_conn) {
            ngtcp2_conn_del(conn->ngtcp2_conn);
            conn->ngtcp2_conn = NULL;
            ngtcp2_crypto_ossl_ctx_del(conn->crypto_ctx);
            conn->crypto_ctx = NULL;
            SSL_free(conn->ssl);
            conn->ssl = NULL;
            _h3_del_conn(t, conn);
            return -1;
        }

        conn->state = 1; /* handshake */

        _h3_flush_http3_to_quic(conn);

        CSILK_LOG_I("H3: new QUIC conn (DCID=%02x%02x%02x%02x)",
                    hd.dcid.data[0],
                    hd.dcid.data[1],
                    hd.dcid.data[2],
                    hd.dcid.data[3]);
    }

    /* Feed the datagram to ngtcp2 */
    struct sockaddr_storage local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    socklen_t local_len = sizeof(local_addr);
    getsockname(t->udp_fd, (struct sockaddr*)&local_addr, &local_len);

    ngtcp2_path_storage ps = _h3_make_path((const struct sockaddr_storage*)from,
                                           fromlen,
                                           (const struct sockaddr*)&local_addr,
                                           local_len);

    ngtcp2_pkt_info pi;
    memset(&pi, 0, sizeof(pi));

    ngtcp2_tstamp ts = csilk_io_hrtime();

    int rrc = ngtcp2_conn_read_pkt_versioned(
        conn->ngtcp2_conn, &ps.path, NGTCP2_PKT_INFO_VERSION, &pi, buf, len, ts);
    if (rrc != 0) {
        /* NGTCP2_ERR_RETRY means we should send a Retry packet */
        if (rrc == NGTCP2_ERR_RETRY) {
            CSILK_LOG_W("H3: retry requested");
        } else {
            CSILK_LOG_W("H3: ngtcp2_conn_read_pkt error: %s", ngtcp2_strerror(rrc));
        }
        /* The conn may have been closed; check if it's still valid */
        if (conn->state == 3) {
            return 0; /* already cleaned up */
        }
    }

    /* Send any pending QUIC packets (handshake responses, etc.) */
    _h3_send_quic_packets(t, conn);

    return 0;
}

/* ngtcp2_crypto_ossl adapter vtable setup (per process, done lazily).
 * Initializes libngtcp2_crypto_ossl to register the AES-128-GCM and
 * SHA-256 EVP singletons used by the generic ngtcp2_crypto_* wrappers.
 * Idempotent; safe to call multiple times. */

static int
_h3_init_crypto_vtable(void)
{
    ngtcp2_crypto_ossl_init();
    return 0;
}

#endif /* CSILK_HAS_NGTCP2 */

/* ===================================================================
 * Public transport API
 * =================================================================== */

/**
 * @brief Create a new HTTP/3 QUIC transport bound to a UDP port.
 *
 * @param port     UDP port to bind (e.g. 443).
 * @param cert_pem  PEM-encoded TLS certificate (NULL = no TLS, test only).
 * @param key_pem   PEM-encoded TLS private key.
 * @return Newly allocated transport handle, or NULL on failure.
 */
csilk_h3_transport_t*
csilk_h3_transport_new(int port, const char* cert_pem, const char* key_pem)
{
    if (port <= 0) {
        return NULL;
    }

    csilk_h3_transport_t* t = calloc(1, sizeof(csilk_h3_transport_t));
    if (!t) {
        return NULL;
    }

    t->conn_count = 0;

#ifdef CSILK_HAS_NGTCP2
    t->udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (t->udp_fd < 0) {
        free(t);
        return NULL;
    }

    int reuse = 1;
    setsockopt(t->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(t->udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(t->udp_fd);
        t->udp_fd = -1;
        free(t);
        return NULL;
    }
    t->udp_nonblocking = 1;

    if (cert_pem && key_pem && _h3_init_ssl_ctx(t, cert_pem, key_pem) != 0) {
        close(t->udp_fd);
        t->udp_fd = -1;
        SSL_CTX_free(t->ssl_ctx);
        t->ssl_ctx = NULL;
        free(t);
        return NULL;
    }

    /* Initialize the shared crypto adapter vtable. */
    _h3_init_crypto_vtable();

    CSILK_LOG_I("HTTP/3: QUIC transport bound UDP port %d", port);
#else
    (void)cert_pem;
    (void)key_pem;
    (void)port;
    CSILK_LOG_W("HTTP/3: CSILK_HAS_NGTCP2 not defined — transport is a no-op");
#endif

    return t;
}

/**
 * @brief Accept new QUIC connections on the transport.
 *
 * Drains all pending UDP datagrams and processes each through ngtcp2.
 *
 * @param t      Transport handle.
 * @param out_conn [out] First newly-accepted connection, or NULL.
 * @return 0 on success (even if no new connections), -1 on error.
 */
int
csilk_h3_transport_accept(csilk_h3_transport_t* t, void** out_conn)
{
    if (!t) {
        return -1;
    }

    struct csilk_h3_conn_s* first_new = NULL;
    int                     rc = 0;

#ifdef CSILK_HAS_NGTCP2
    if (t->udp_fd < 0) {
        return 0;
    }

    struct sockaddr_storage from;
    socklen_t               fromlen;

    while (1) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(
            t->udp_fd, t->recv_buf, sizeof(t->recv_buf), 0, (struct sockaddr*)&from, &fromlen);
        if (n <= 0) {
            if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                CSILK_LOG_W("H3: recvfrom error: %s", strerror(errno));
                rc = -1;
            }
            break; /* no more datagrams */
        }
        int prev_count = t->conn_count;
        _h3_process_datagram(t, t->recv_buf, (size_t)n, (const struct sockaddr*)&from, fromlen);
        if (t->conn_count > prev_count && !first_new) {
            first_new = t->conns[prev_count];
        }
    }
#else
    (void)out_conn;
    return 0;
#endif

    if (out_conn) {
        *out_conn = first_new;
    }
    return rc;
}

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
int
csilk_h3_transport_send(
    csilk_h3_transport_t* t, void* conn, uint64_t stream_id, const uint8_t* data, size_t len)
{
    if (!t || !conn || !data || len == 0) {
        return -1;
    }

#ifdef CSILK_HAS_NGTCP2
    struct csilk_h3_conn_s* h3c = (struct csilk_h3_conn_s*)conn;
    if (!h3c->nghttp3_conn || !h3c->ngtcp2_conn) {
        return -1;
    }

    /* Build an nghttp3 vec pointing at the data, emit it as a stream
     * write via nghttp3, then push the resulting QUIC payload to ngtcp2
     * and send any pending packets. */
    nghttp3_vec iov = {
        .base = (uint8_t*)data,
        .len = len,
    };
    int64_t       out_stream_id = 0;
    int           out_fin = 0;
    nghttp3_ssize nvec =
        nghttp3_conn_writev_stream(h3c->nghttp3_conn, &out_stream_id, &out_fin, &iov, 1);
    if (nvec < 0) {
        return -1;
    }
    if (nvec == 0) {
        return 0; /* nothing to send */
    }

    /* Write the stream data into ngtcp2, then send QUIC packets.
     * Convert nghttp3_vec → ngtcp2_vec (same layout, different type). */
    uint8_t    pktbuf[NGTCP2_MAX_UDP_PAYLOAD_SIZE + 64];
    ngtcp2_vec nqvec;
    nqvec.base = iov.base;
    nqvec.len = iov.len;
    ngtcp2_ssize pdatalen = 0;
    ngtcp2_ssize written = ngtcp2_conn_writev_stream(h3c->ngtcp2_conn,
                                                     NULL,
                                                     NULL,
                                                     pktbuf,
                                                     sizeof(pktbuf),
                                                     &pdatalen,
                                                     out_fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0,
                                                     out_stream_id,
                                                     &nqvec,
                                                     1,
                                                     csilk_io_hrtime());
    if (written >= 0 && pdatalen > 0) {
        nghttp3_conn_add_write_offset(h3c->nghttp3_conn, out_stream_id, (size_t)pdatalen);
    }
    _h3_send_quic_packets(t, h3c);
    return 0;
#else
    (void)stream_id;
    (void)data;
    (void)len;
    return -ENOTSUP;
#endif
}

/**
 * @brief Receive data from an HTTP/3 stream.
 *
 * @param t        Transport handle.
 * @param conn     Connection handle.
 * @param stream_id  Stream ID.
 * @param buf      [out] Receive buffer.
 * @param buf_len  Buffer capacity.
 * @return Number of bytes received, or -1 on error.
 */
int
csilk_h3_transport_recv(
    csilk_h3_transport_t* t, void* conn, uint64_t stream_id, uint8_t* buf, size_t buf_len)
{
    if (!t || !conn || !buf || buf_len == 0) {
        return -1;
    }

#ifdef CSILK_HAS_NGTCP2
    struct csilk_h3_conn_s* h3c = (struct csilk_h3_conn_s*)conn;
    if (!h3c->nghttp3_conn) {
        return -1;
    }

    /* Drain the nghttp3 session's pending data for this stream. */
    uint8_t       out[4096];
    nghttp3_ssize nr = nghttp3_conn_read_stream(
        h3c->nghttp3_conn, (int64_t)stream_id, out, sizeof(out), 0 /* fin */);
    if (nr < 0) {
        return -1;
    }
    size_t copy = (size_t)nr < buf_len ? (size_t)nr : buf_len;
    if (copy > 0) {
        memcpy(buf, out, copy);
    }
    return (int)copy;
#else
    (void)stream_id;
    (void)buf;
    (void)buf_len;
    return -ENOTSUP;
#endif
}

/**
 * @brief Close an HTTP/3 stream.
 *
 * @param t          Transport handle.
 * @param conn       Connection handle.
 * @param stream_id  Stream ID to close.
 * @return 0 on success, -1 on error.
 */
int
csilk_h3_transport_close_stream(csilk_h3_transport_t* t, void* conn, uint64_t stream_id)
{
    if (!t || !conn) {
        return -1;
    }

#ifdef CSILK_HAS_NGTCP2
    struct csilk_h3_conn_s* h3c = (struct csilk_h3_conn_s*)conn;
    if (!h3c->nghttp3_conn) {
        return -1;
    }
    return nghttp3_conn_close_stream(h3c->nghttp3_conn, (int64_t)stream_id, 0) == 0 ? 0 : -1;
#else
    (void)stream_id;
    return -ENOTSUP;
#endif
}

/**
 * @brief Close a QUIC connection.
 *
 * @param t     Transport handle.
 * @param conn  Connection handle.
 * @return 0 on success, -1 on error.
 */
int
csilk_h3_transport_close_conn(csilk_h3_transport_t* t, void* conn)
{
    if (!t || !conn) {
        return -1;
    }

#ifdef CSILK_HAS_NGTCP2
    _h3_del_conn(t, (struct csilk_h3_conn_s*)conn);
#else
    (void)t;
    free(conn);
#endif
    return 0;
}

/**
 * @brief Destroy the transport and release all resources.
 *
 * @param t  Transport handle to free.
 */
void
csilk_h3_transport_destroy(csilk_h3_transport_t* t)
{
    if (!t) {
        return;
    }

#ifdef CSILK_HAS_NGTCP2
    while (t->conn_count > 0) {
        _h3_del_conn(t, t->conns[t->conn_count - 1]);
    }
    t->conn_count = 0;
    if (t->ssl_ctx) {
        SSL_CTX_free(t->ssl_ctx);
        t->ssl_ctx = NULL;
    }
    if (t->udp_fd >= 0) {
        close(t->udp_fd);
        t->udp_fd = -1;
    }
#endif

    free(t);
}

/* ===================================================================
 * TLS setup helper
 * =================================================================== */

#ifdef CSILK_HAS_NGTCP2
static int
_h3_init_ssl_ctx(csilk_h3_transport_t* t, const char* cert_pem, const char* key_pem)
{
    t->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!t->ssl_ctx) {
        return -1;
    }
    SSL_CTX_set_min_proto_version(t->ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_session_cache_mode(t->ssl_ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_max_early_data(t->ssl_ctx, 0xffffffff);

    BIO*      cert_bio = BIO_new_mem_buf(cert_pem, (int)strlen(cert_pem));
    BIO*      key_bio = BIO_new_mem_buf(key_pem, (int)strlen(key_pem));
    X509*     x509 = NULL;
    EVP_PKEY* key = NULL;
    int       rc = -1;

    if (cert_bio && key_bio) {
        x509 = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
        key = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
        if (x509 && key) {
            SSL_CTX_use_certificate(t->ssl_ctx, x509);
            SSL_CTX_use_PrivateKey(t->ssl_ctx, key);
            rc = 0;
        }
    }
    if (x509) {
        X509_free(x509);
    }
    if (key) {
        EVP_PKEY_free(key);
    }
    BIO_free_all(cert_bio);
    BIO_free_all(key_bio);
    return rc;
}
#endif /* CSILK_HAS_NGTCP2 */

/**
 * @brief Create a QUIC-enabled SSL object for ngtcp2's OpenSSL crypto adapter.
 *
 * Configures the SSL object for TLS 1.3 + QUIC (RFC 9001):
 *  - TLS 1.3 only (min = max = TLS1_3_VERSION)
 *
 * The QUIC transport parameters and TLS callbacks are wired by the
 * ngtcp2_crypto_ossl adapter via SSL_set_quic_tls_cbs() when
 * ngtcp2_crypto_ossl_configure_server_session() is called.
 *
 * @param ssl_ctx  TLS context (from csilk_h3_transport_new).
 * @return A new SSL* configured for QUIC, or NULL on failure.
 */
#ifdef CSILK_HAS_NGTCP2
SSL*
csilk_h3_quic_tls_new(SSL_CTX* ssl_ctx)
{
    if (!ssl_ctx) {
        return NULL;
    }

    SSL* ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        return NULL;
    }

    /* Enforce TLS 1.3 only for QUIC (RFC 9001 mandates TLS 1.3). */
    SSL_set_min_proto_version(ssl, TLS1_3_VERSION);
    SSL_set_max_proto_version(ssl, TLS1_3_VERSION);
    SSL_set_max_early_data(ssl, 0xffffffff);

    return ssl;
}
#endif /* CSILK_HAS_NGTCP2 */

/* ===================================================================
 * vtable registration (matches csilk_quic_transport_t from server.h)
 * =================================================================== */

static int
_h3_on_stream_recv(csilk_server_t* server, int64_t stream_id, const uint8_t* data, size_t len)
{
    if (!server || !server->h3_transport) {
        return 0;
    }
#ifdef CSILK_HAS_NGTCP2
    csilk_h3_transport_t* t = (csilk_h3_transport_t*)server->h3_transport;
    for (int i = 0; i < t->conn_count; i++) {
        struct csilk_h3_conn_s* c = t->conns[i];
        if (c && c->nghttp3_conn) {
            nghttp3_ssize consumed =
                nghttp3_conn_read_stream(c->nghttp3_conn, stream_id, data, len, 0);
            if (consumed > 0) {
                nghttp3_conn_add_write_offset(c->nghttp3_conn, stream_id, (size_t)consumed);
                return 0;
            }
        }
    }
#else
    (void)stream_id;
    (void)data;
    (void)len;
#endif
    return 0;
}

static int
_h3_on_stream_send(csilk_server_t* server, int64_t stream_id, const uint8_t* data, size_t len)
{
    if (!server || !server->h3_transport) {
        return 0;
    }
#ifdef CSILK_HAS_NGTCP2
    csilk_h3_transport_t* t = (csilk_h3_transport_t*)server->h3_transport;
    for (int i = 0; i < t->conn_count; i++) {
        struct csilk_h3_conn_s* c = t->conns[i];
        if (c && c->nghttp3_conn) {
            csilk_h3_session_send_body(c->nghttp3_conn, stream_id, data, len);
            _h3_flush_http3_to_quic(c);
            _h3_send_quic_packets(t, c);
            return 0;
        }
    }
#else
    (void)stream_id;
    (void)data;
    (void)len;
#endif
    return 0;
}

/**
 * @brief Enable HTTP/3 on a csilk_server_t.
 *
 * @param server  Server instance (csilk_server_t*).
 * @param port    UDP port for QUIC (e.g. 443).
 * @return 0 on success, -1 on failure.
 */
int
csilk_h3_set_enabled(void* server, int port)
{
    if (!server || port <= 0) {
        return -1;
    }
    csilk_server_t*               srv = (csilk_server_t*)server;
    static csilk_quic_transport_t h3_vtable = {
        .name = "ngtcp2",
        .on_stream_recv = _h3_on_stream_recv,
        .on_stream_send = _h3_on_stream_send,
    };
    csilk_server_set_quic_transport(srv, &h3_vtable);
    csilk_h3_transport_t* t = csilk_h3_transport_new(port, NULL, NULL);
    if (t) {
        t->server = srv;
        srv->h3_transport = t;
    }
    CSILK_LOG_I("HTTP/3: enabled on server, UDP port %d", port);
    return 0;
}
