/**
 * @file test_h3_e2e.c
 * @brief Real UDP/QUIC + TLS 1.3 + HTTP/3 end-to-end integration test.
 *
 * Exercises a real ngtcp2 + ngtcp2_crypto_ossl + nghttp3 client talking over
 * loopback IPv4 UDP sockets to a live csilk_server_t with HTTP/3 enabled:
 *   1. Ephemeral self-signed ECDSA P-256 certificate & TLS 1.3 ALPN "h3" handshake
 *   2. GET /h3/hello -> 200 OK + response header X-H3-Server + body "hello-http3"
 *   3. POST /h3/echo with request header + body -> 201 Created + echoed payload
 *   4. GET /h3/stream -> 200 OK streaming csilk_response_write()/end() ("chunk-a:chunk-b")
 *   5. GET /h3/missing -> 404 Not Found
 *   6. Multi-stream multiplexing over a single QUIC connection + clean teardown under ASAN/LSAN
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/sync.h"
#include "csilk/core/sys_io.h"
#include "csilk/csilk.h"
#include "csilk/protocols/h3.h"

#ifdef CSILK_HAS_NGTCP2
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* ====================================================================
 * Ephemeral Self-Signed ECDSA P-256 Cert/Key Generator
 * ==================================================================== */

static void
generate_test_cert_and_key(char** out_cert_pem, char** out_key_pem)
{
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    assert(pkey != NULL);

    X509* x509 = X509_new();
    assert(x509 != NULL);
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 86400);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    assert(X509_sign(x509, pkey, EVP_sha256()) > 0);

    BIO* cbio = BIO_new(BIO_s_mem());
    BIO* kbio = BIO_new(BIO_s_mem());
    assert(cbio != NULL && kbio != NULL);
    assert(PEM_write_bio_X509(cbio, x509) == 1);
    assert(PEM_write_bio_PrivateKey(kbio, pkey, NULL, NULL, 0, NULL, NULL) == 1);

    BUF_MEM* cmem = NULL;
    BUF_MEM* kmem = NULL;
    BIO_get_mem_ptr(cbio, &cmem);
    BIO_get_mem_ptr(kbio, &kmem);

    *out_cert_pem = malloc(cmem->length + 1);
    *out_key_pem = malloc(kmem->length + 1);
    assert(*out_cert_pem != NULL && *out_key_pem != NULL);
    memcpy(*out_cert_pem, cmem->data, cmem->length);
    (*out_cert_pem)[cmem->length] = '\0';
    memcpy(*out_key_pem, kmem->data, kmem->length);
    (*out_key_pem)[kmem->length] = '\0';

    BIO_free_all(cbio);
    BIO_free_all(kbio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
}

static int
reserve_free_udp_port(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int
reserve_free_tcp_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

/* ====================================================================
 * Server Routes
 * ==================================================================== */

static void
h3_hello_handler(csilk_ctx_t* c)
{
    csilk_set_header(c, "X-H3-Server", "csilk");
    csilk_string(c, 200, "hello-http3");
}

static void
h3_echo_handler(csilk_ctx_t* c)
{
    const char* tag = csilk_get_header(c, "x-req-tag");
    if (tag) {
        csilk_set_header(c, "X-Echo-Tag", tag);
    }
    size_t      blen = 0;
    const char* body = csilk_get_body(c, &blen);
    char        buf[256] = {0};
    if (body && blen > 0) {
        size_t n = blen < sizeof(buf) - 1 ? blen : sizeof(buf) - 1;
        memcpy(buf, body, n);
        buf[n] = '\0';
    }
    csilk_string(c, 201, buf);
}

static void
h3_stream_handler(csilk_ctx_t* c)
{
    csilk_set_status(c, 200);
    csilk_response_write(c, (const uint8_t*)"chunk-a:", 8);
    csilk_response_write(c, (const uint8_t*)"chunk-b", 7);
    csilk_response_end(c);
}

typedef struct {
    csilk_server_t* server;
    int             tcp_port;
} h3_srv_thread_ctx_t;

static void
h3_server_thread_fn(void* arg)
{
    h3_srv_thread_ctx_t* sctx = (h3_srv_thread_ctx_t*)arg;
    csilk_server_run(sctx->server, sctx->tcp_port);
}

/* ====================================================================
 * Real QUIC + TLS 1.3 + nghttp3 Client Harness
 * ==================================================================== */

typedef struct {
    int64_t stream_id;
    int     status;
    char    custom_hdr[64];
    char    body[512];
    size_t  body_len;
    int     done;
} h3_client_resp_t;

typedef struct {
    int                     udp_fd;
    struct sockaddr_storage remote_addr;
    socklen_t               remote_len;
    struct sockaddr_storage local_addr;
    socklen_t               local_len;
    SSL_CTX*                ssl_ctx;
    SSL*                    ssl;
    ngtcp2_crypto_ossl_ctx* crypto_ctx;
    ngtcp2_crypto_conn_ref  conn_ref;
    ngtcp2_conn*            qconn;
    nghttp3_conn*           h3conn;
    int                     handshake_done;
    h3_client_resp_t        resps[4];
    size_t                  post_offset;
} h3_client_t;

static ngtcp2_conn*
client_get_conn(ngtcp2_crypto_conn_ref* ref)
{
    h3_client_t* cl = (h3_client_t*)ref->user_data;
    return cl ? cl->qconn : NULL;
}

static void
client_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* ctx)
{
    (void)ctx;
    RAND_bytes(dest, (int)destlen);
}

static int
client_get_new_cid(ngtcp2_conn*                  conn,
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

static int
client_handshake_completed(ngtcp2_conn* conn, void* user_data)
{
    (void)conn;
    h3_client_t* cl = (h3_client_t*)user_data;
    cl->handshake_done = 1;
    return 0;
}

static int
client_recv_stream_data(ngtcp2_conn*   conn,
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
    h3_client_t* cl = (h3_client_t*)user_data;
    if (!cl || !cl->h3conn) {
        return 0;
    }
    int           fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0;
    nghttp3_ssize nconsumed = nghttp3_conn_read_stream(cl->h3conn, stream_id, data, datalen, fin);
    if (nconsumed < 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if (nconsumed > 0) {
        ngtcp2_conn_extend_max_stream_offset(conn, stream_id, (uint64_t)nconsumed);
        ngtcp2_conn_extend_max_offset(conn, (uint64_t)nconsumed);
    }
    return 0;
}

static int
client_acked_stream_data_offset(ngtcp2_conn* conn,
                                int64_t      stream_id,
                                uint64_t     offset,
                                uint64_t     datalen,
                                void*        user_data,
                                void*        stream_user_data)
{
    (void)conn;
    (void)offset;
    (void)stream_user_data;
    h3_client_t* cl = (h3_client_t*)user_data;
    if (cl && cl->h3conn && datalen > 0) {
        if (nghttp3_conn_add_ack_offset(cl->h3conn, stream_id, datalen) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }
    return 0;
}

static h3_client_resp_t*
find_client_resp(h3_client_t* cl, int64_t stream_id)
{
    for (int i = 0; i < 4; i++) {
        if (cl->resps[i].stream_id == stream_id) {
            return &cl->resps[i];
        }
    }
    return NULL;
}

static int
cl_h3_recv_header(nghttp3_conn*  conn,
                  int64_t        stream_id,
                  int32_t        token,
                  nghttp3_rcbuf* name,
                  nghttp3_rcbuf* value,
                  uint8_t        flags,
                  void*          conn_user_data,
                  void*          stream_user_data)
{
    (void)conn;
    (void)token;
    (void)flags;
    (void)stream_user_data;
    h3_client_t*      cl = (h3_client_t*)conn_user_data;
    h3_client_resp_t* r = find_client_resp(cl, stream_id);
    if (!r) {
        return 0;
    }
    nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
    if (n.len == 7 && memcmp(n.base, ":status", 7) == 0 && v.len < 16) {
        char tmp[16] = {0};
        memcpy(tmp, v.base, v.len);
        r->status = atoi(tmp);
    } else if ((n.len == 11 && memcmp(n.base, "x-h3-server", 11) == 0) ||
               (n.len == 10 && memcmp(n.base, "x-echo-tag", 10) == 0)) {
        size_t cp = v.len < sizeof(r->custom_hdr) - 1 ? v.len : sizeof(r->custom_hdr) - 1;
        memcpy(r->custom_hdr, v.base, cp);
        r->custom_hdr[cp] = '\0';
    }
    return 0;
}

static int
cl_h3_recv_data(nghttp3_conn*  conn,
                int64_t        stream_id,
                const uint8_t* data,
                size_t         datalen,
                void*          conn_user_data,
                void*          stream_user_data)
{
    (void)conn;
    (void)stream_user_data;
    h3_client_t*      cl = (h3_client_t*)conn_user_data;
    h3_client_resp_t* r = find_client_resp(cl, stream_id);
    if (!r || !data || datalen == 0) {
        return 0;
    }
    size_t space = sizeof(r->body) - 1 - r->body_len;
    size_t cp = datalen < space ? datalen : space;
    memcpy(r->body + r->body_len, data, cp);
    r->body_len += cp;
    r->body[r->body_len] = '\0';
    return 0;
}

static int
cl_h3_end_stream(nghttp3_conn* conn,
                 int64_t       stream_id,
                 void*         conn_user_data,
                 void*         stream_user_data)
{
    (void)conn;
    (void)stream_user_data;
    h3_client_t*      cl = (h3_client_t*)conn_user_data;
    h3_client_resp_t* r = find_client_resp(cl, stream_id);
    if (r) {
        r->done = 1;
    }
    return 0;
}

static int
cl_h3_deferred_consume(nghttp3_conn* conn,
                       int64_t       stream_id,
                       size_t        consumed,
                       void*         conn_user_data,
                       void*         stream_user_data)
{
    (void)conn;
    (void)stream_user_data;
    h3_client_t* cl = (h3_client_t*)conn_user_data;
    if (cl && cl->qconn && consumed > 0) {
        ngtcp2_conn_extend_max_stream_offset(cl->qconn, stream_id, (uint64_t)consumed);
        ngtcp2_conn_extend_max_offset(cl->qconn, (uint64_t)consumed);
    }
    return 0;
}

static const char g_post_payload[] = "quic-post-body-99";

static nghttp3_ssize
cl_h3_read_post_body(nghttp3_conn* conn,
                     int64_t       stream_id,
                     nghttp3_vec*  vec,
                     size_t        veccnt,
                     uint32_t*     pflags,
                     void*         conn_user_data,
                     void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)stream_user_data;
    h3_client_t* cl = (h3_client_t*)conn_user_data;
    size_t       total = sizeof(g_post_payload) - 1;
    if (cl->post_offset >= total) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    if (veccnt == 0) {
        return 0;
    }
    size_t rem = total - cl->post_offset;
    vec[0].base = (uint8_t*)(g_post_payload + cl->post_offset);
    vec[0].len = rem;
    cl->post_offset += rem;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

static void
client_ngtcp2_log(void* user_data, const char* fmt, ...)
{
    (void)user_data;
    if (!getenv("H3_E2E_DEBUG")) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void
client_flush_quic(h3_client_t* cl)
{
    uint8_t pktbuf[NGTCP2_MAX_UDP_PAYLOAD_SIZE + 64];

    if (cl->h3conn && cl->handshake_done) {
        while (1) {
            int64_t       sid = -1;
            int           fin = 0;
            nghttp3_vec   vec[16];
            nghttp3_ssize nvec = nghttp3_conn_writev_stream(cl->h3conn, &sid, &fin, vec, 16);
            if (nvec < 0) {
                break;
            }
            if (nvec == 0 && (!fin || sid < 0)) {
                break;
            }
            ngtcp2_vec qvec[16];
            for (nghttp3_ssize i = 0; i < nvec; i++) {
                qvec[i].base = vec[i].base;
                qvec[i].len = vec[i].len;
            }
            ngtcp2_ssize pdatalen = 0;
            uint32_t     wflags = fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0;
            ngtcp2_ssize w = ngtcp2_conn_writev_stream(cl->qconn,
                                                       NULL,
                                                       NULL,
                                                       pktbuf,
                                                       sizeof(pktbuf),
                                                       &pdatalen,
                                                       wflags,
                                                       sid,
                                                       nvec > 0 ? qvec : NULL,
                                                       (size_t)nvec,
                                                       csilk_io_hrtime());
            if (w < 0) {
                break;
            }
            if (w == 0 && pdatalen <= 0) {
                break;
            }
            if (pdatalen > 0) {
                nghttp3_conn_add_write_offset(cl->h3conn, sid, (size_t)pdatalen);
            } else if (nvec == 0 && fin) {
                nghttp3_conn_add_write_offset(cl->h3conn, sid, 0);
            }
            if (w > 0) {
                ssize_t sr = send(cl->udp_fd, pktbuf, (size_t)w, 0);
                if (sr > 0) {
                    ngtcp2_conn_update_pkt_tx_time(cl->qconn, csilk_io_hrtime());
                }
            }
        }
    }

    for (int i = 0; i < 64; i++) {
        ngtcp2_pkt_info pi = {0};
        ngtcp2_ssize    w = ngtcp2_conn_write_pkt_versioned(cl->qconn,
                                                         NULL,
                                                         NGTCP2_PKT_INFO_VERSION,
                                                         &pi,
                                                         pktbuf,
                                                         sizeof(pktbuf),
                                                         csilk_io_hrtime());
        if (w <= 0) {
            break;
        }
        ssize_t sr = send(cl->udp_fd, pktbuf, (size_t)w, 0);
        if (sr > 0) {
            ngtcp2_conn_update_pkt_tx_time(cl->qconn, csilk_io_hrtime());
        }
    }
}

static void
client_pump_io(h3_client_t* cl, int timeout_ms)
{
    client_flush_quic(cl);
    struct pollfd pfd = {.fd = cl->udp_fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        return;
    }
    uint8_t buf[65536];
    while (1) {
        ssize_t n = recv(cl->udp_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        ngtcp2_path path;
        path.local.addr = (ngtcp2_sockaddr*)&cl->local_addr;
        path.local.addrlen = cl->local_len;
        path.remote.addr = (ngtcp2_sockaddr*)&cl->remote_addr;
        path.remote.addrlen = cl->remote_len;
        ngtcp2_pkt_info pi = {0};
        int             rc = ngtcp2_conn_read_pkt_versioned(
            cl->qconn, &path, NGTCP2_PKT_INFO_VERSION, &pi, buf, (size_t)n, csilk_io_hrtime());
        assert(rc == 0 || rc == NGTCP2_ERR_DRAINING || rc == NGTCP2_ERR_CLOSING);
    }
    client_flush_quic(cl);
}

static void
test_h3_real_udp_quic_e2e(void)
{
    printf("Testing real UDP/QUIC TLS 1.3 HTTP/3 end-to-end requests...\n");

    ngtcp2_crypto_ossl_init();

    char* cert_pem = NULL;
    char* key_pem = NULL;
    generate_test_cert_and_key(&cert_pem, &key_pem);

    int tcp_port = reserve_free_tcp_port();
    int h3_port = reserve_free_udp_port();

    csilk_router_t* router = csilk_router_new();
    assert(router != NULL);
    csilk_handler_t h_hello[] = {h3_hello_handler};
    csilk_handler_t h_echo[] = {h3_echo_handler};
    csilk_handler_t h_stream[] = {h3_stream_handler};
    assert(csilk_router_add(router, "GET", "/h3/hello", h_hello, 1) == 0);
    assert(csilk_router_add(router, "POST", "/h3/echo", h_echo, 1) == 0);
    assert(csilk_router_add(router, "GET", "/h3/stream", h_stream, 1) == 0);

    csilk_server_t* srv = csilk_server_new(router);
    assert(srv != NULL);
    csilk_server_config_t cfg = {
        .worker_threads = 1,
        .idle_timeout_ms = 5000,
        .max_body_size = 1024 * 1024,
        .max_header_size = 64 * 1024,
        .listen_backlog = 32,
    };
    csilk_server_set_config(srv, &cfg);
    assert(csilk_h3_set_enabled_tls(srv, h3_port, cert_pem, key_pem) == 0);

    h3_srv_thread_ctx_t sctx = {.server = srv, .tcp_port = tcp_port};
    csilk_thread_t      srv_th;
    assert(csilk_thread_create(&srv_th, h3_server_thread_fn, &sctx) == 0);

    /* Wait until TCP control listener is ready (implies worker 0 H3 UDP socket is also bound) */
    for (int i = 0; i < 100; i++) {
        int                pfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ta = {0};
        ta.sin_family = AF_INET;
        ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ta.sin_port = htons((uint16_t)tcp_port);
        if (connect(pfd, (struct sockaddr*)&ta, sizeof(ta)) == 0) {
            close(pfd);
            break;
        }
        close(pfd);
        csilk_io_sleep(10);
    }

    /* Setup client UDP socket connected to 127.0.0.1:h3_port */
    h3_client_t cl = {0};
    for (int i = 0; i < 4; i++) {
        cl.resps[i].stream_id = -1;
    }
    cl.udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(cl.udp_fd >= 0);
    struct sockaddr_in* raddr = (struct sockaddr_in*)&cl.remote_addr;
    raddr->sin_family = AF_INET;
    raddr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    raddr->sin_port = htons((uint16_t)h3_port);
    cl.remote_len = sizeof(*raddr);
    assert(connect(cl.udp_fd, (struct sockaddr*)&cl.remote_addr, cl.remote_len) == 0);
    cl.local_len = sizeof(cl.local_addr);
    assert(getsockname(cl.udp_fd, (struct sockaddr*)&cl.local_addr, &cl.local_len) == 0);

    /* Configure TLS 1.3 QUIC client SSL */
    cl.ssl_ctx = SSL_CTX_new(TLS_client_method());
    assert(cl.ssl_ctx != NULL);
    SSL_CTX_set_min_proto_version(cl.ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(cl.ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(cl.ssl_ctx, SSL_VERIFY_NONE, NULL);
    static const unsigned char alpn[] = {2, 'h', '3'};
    assert(SSL_CTX_set_alpn_protos(cl.ssl_ctx, alpn, sizeof(alpn)) == 0);

    cl.ssl = SSL_new(cl.ssl_ctx);
    assert(cl.ssl != NULL);
    SSL_set_connect_state(cl.ssl);
    SSL_set_tlsext_host_name(cl.ssl, "localhost");
    assert(ngtcp2_crypto_ossl_configure_client_session(cl.ssl) == 0);
    assert(ngtcp2_crypto_ossl_ctx_new(&cl.crypto_ctx, cl.ssl) == 0);

    cl.conn_ref.get_conn = client_get_conn;
    cl.conn_ref.user_data = &cl;
    SSL_set_app_data(cl.ssl, &cl.conn_ref);

    ngtcp2_cid dcid, scid;
    dcid.datalen = 8;
    scid.datalen = 8;
    RAND_bytes(dcid.data, (int)dcid.datalen);
    RAND_bytes(scid.data, (int)scid.datalen);

    ngtcp2_path path = {
        .local = {.addr = (ngtcp2_sockaddr*)&cl.local_addr,  .addrlen = cl.local_len },
        .remote = {.addr = (ngtcp2_sockaddr*)&cl.remote_addr, .addrlen = cl.remote_len},
    };

    ngtcp2_callbacks cbs = {0};
    cbs.client_initial = ngtcp2_crypto_client_initial_cb;
    cbs.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cbs.handshake_completed = client_handshake_completed;
    cbs.recv_stream_data = client_recv_stream_data;
    cbs.acked_stream_data_offset = client_acked_stream_data_offset;
    cbs.update_key = ngtcp2_crypto_update_key_cb;
    cbs.rand = client_rand;
    cbs.get_new_connection_id2 = client_get_new_cid;
    cbs.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cbs.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cbs.encrypt = ngtcp2_crypto_encrypt_cb;
    cbs.decrypt = ngtcp2_crypto_decrypt_cb;
    cbs.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cbs.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cbs.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cbs.recv_retry = ngtcp2_crypto_recv_retry_cb;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = csilk_io_hrtime();
    settings.log_printf = client_ngtcp2_log;

    ngtcp2_transport_params tp;
    ngtcp2_transport_params_default(&tp);
    tp.initial_max_streams_bidi = 128;
    tp.initial_max_streams_uni = 128;
    tp.initial_max_stream_data_bidi_local = 256 * 1024;
    tp.initial_max_stream_data_bidi_remote = 256 * 1024;
    tp.initial_max_stream_data_uni = 256 * 1024;
    tp.initial_max_data = 1024 * 1024;
    tp.max_idle_timeout = 30 * NGTCP2_SECONDS;

    assert(
        ngtcp2_conn_client_new(
            &cl.qconn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cbs, &settings, &tp, NULL, &cl) ==
        0);
    ngtcp2_conn_set_tls_native_handle(cl.qconn, cl.crypto_ctx);

    /* Pump QUIC + TLS 1.3 handshake */
    for (int i = 0; i < 100 && !cl.handshake_done; i++) {
        client_pump_io(&cl, 20);
    }
    assert(cl.handshake_done == 1);

    const unsigned char* neg_alpn = NULL;
    unsigned int         neg_alpn_len = 0;
    SSL_get0_alpn_selected(cl.ssl, &neg_alpn, &neg_alpn_len);
    assert(neg_alpn != NULL && neg_alpn_len == 2 && memcmp(neg_alpn, "h3", 2) == 0);

    /* Create nghttp3 client session + control & QPACK unidirectional streams */
    nghttp3_callbacks h3cbs = {
        .recv_header = cl_h3_recv_header,
        .recv_data = cl_h3_recv_data,
        .end_stream = cl_h3_end_stream,
        .deferred_consume = cl_h3_deferred_consume,
    };
    nghttp3_settings h3settings;
    nghttp3_settings_default(&h3settings);
    assert(nghttp3_conn_client_new(&cl.h3conn, &h3cbs, &h3settings, NULL, &cl) == 0);

    int64_t ctrl_sid = -1, qenc_sid = -1, qdec_sid = -1;
    assert(ngtcp2_conn_open_uni_stream(cl.qconn, &ctrl_sid, NULL) == 0);
    assert(ngtcp2_conn_open_uni_stream(cl.qconn, &qenc_sid, NULL) == 0);
    assert(ngtcp2_conn_open_uni_stream(cl.qconn, &qdec_sid, NULL) == 0);
    assert(nghttp3_conn_bind_control_stream(cl.h3conn, ctrl_sid) == 0);
    assert(nghttp3_conn_bind_qpack_streams(cl.h3conn, qenc_sid, qdec_sid) == 0);

    /* Open 4 concurrent bidi HTTP/3 request streams */
    int64_t s0 = -1, s1 = -1, s2 = -1, s3 = -1;
    assert(ngtcp2_conn_open_bidi_stream(cl.qconn, &s0, NULL) == 0);
    assert(ngtcp2_conn_open_bidi_stream(cl.qconn, &s1, NULL) == 0);
    assert(ngtcp2_conn_open_bidi_stream(cl.qconn, &s2, NULL) == 0);
    assert(ngtcp2_conn_open_bidi_stream(cl.qconn, &s3, NULL) == 0);
    cl.resps[0].stream_id = s0;
    cl.resps[1].stream_id = s1;
    cl.resps[2].stream_id = s2;
    cl.resps[3].stream_id = s3;

    nghttp3_nv req0[] = {
        {(uint8_t*)":method",    (uint8_t*)"GET",       7,  3, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",     7,  5, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/h3/hello", 5,  9, NGHTTP3_NV_FLAG_NONE},
    };
    assert(nghttp3_conn_submit_request(cl.h3conn, s0, req0, 4, NULL, NULL) == 0);

    nghttp3_nv req1[] = {
        {(uint8_t*)":method",    (uint8_t*)"POST",      7,  4, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",     7,  5, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/h3/echo",  5,  8, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)"x-req-tag",  (uint8_t*)"e2e-h3",    9,  6, NGHTTP3_NV_FLAG_NONE},
    };
    nghttp3_data_reader post_dr = {.read_data = cl_h3_read_post_body};
    assert(nghttp3_conn_submit_request(cl.h3conn, s1, req1, 5, &post_dr, NULL) == 0);

    nghttp3_nv req2[] = {
        {(uint8_t*)":method",    (uint8_t*)"GET",        7,  3,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",      7,  5,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost",  10, 9,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/h3/stream", 5,  10, NGHTTP3_NV_FLAG_NONE},
    };
    assert(nghttp3_conn_submit_request(cl.h3conn, s2, req2, 4, NULL, NULL) == 0);

    nghttp3_nv req3[] = {
        {(uint8_t*)":method",    (uint8_t*)"GET",         7,  3,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",       7,  5,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost",   10, 9,  NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/h3/missing", 5,  11, NGHTTP3_NV_FLAG_NONE},
    };
    assert(nghttp3_conn_submit_request(cl.h3conn, s3, req3, 4, NULL, NULL) == 0);

    /* Pump until all 4 responses complete over UDP/QUIC */
    for (int i = 0; i < 150; i++) {
        if (cl.resps[0].done && cl.resps[1].done && cl.resps[2].done && cl.resps[3].done) {
            break;
        }
        client_pump_io(&cl, 20);
    }

    assert(cl.resps[0].done == 1);
    assert(cl.resps[0].status == 200);
    assert(strcmp(cl.resps[0].custom_hdr, "csilk") == 0);
    assert(strcmp(cl.resps[0].body, "hello-http3") == 0);

    assert(cl.resps[1].done == 1);
    assert(cl.resps[1].status == 201);
    assert(strcmp(cl.resps[1].custom_hdr, "e2e-h3") == 0);
    assert(strcmp(cl.resps[1].body, g_post_payload) == 0);

    assert(cl.resps[2].done == 1);
    assert(cl.resps[2].status == 200);
    assert(strcmp(cl.resps[2].body, "chunk-a:chunk-b") == 0);

    assert(cl.resps[3].done == 1);
    assert(cl.resps[3].status == 404);

    /* Clean teardown */
    nghttp3_conn_del(cl.h3conn);
    ngtcp2_conn_del(cl.qconn);
    ngtcp2_crypto_ossl_ctx_del(cl.crypto_ctx);
    SSL_set_app_data(cl.ssl, NULL);
    SSL_free(cl.ssl);
    SSL_CTX_free(cl.ssl_ctx);
    close(cl.udp_fd);

    csilk_server_stop(srv);
    assert(csilk_thread_join(&srv_th) == 0);
    csilk_server_free(srv);
    csilk_router_free(router);
    free(cert_pem);
    free(key_pem);

    printf("test_h3_real_udp_quic_e2e: PASS\n");
}
#endif /* CSILK_HAS_NGTCP2 */

int
main(void)
{
#ifdef CSILK_HAS_NGTCP2
    test_h3_real_udp_quic_e2e();
#else
    printf("Skipping test_h3_e2e (CSILK_HAS_NGTCP2 not enabled)\n");
#endif
    return 0;
}
