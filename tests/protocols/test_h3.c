#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/protocols/h3.h"
#include "csilk/test/test.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "core/ctx/ctx_internal.h"
#include "protocols/http3_internal.h"

#ifdef CSILK_HAS_NGTCP2
#include <openssl/ssl.h>
#endif

static int   g_h3_handler_invoked = 0;
static char  g_h3_received_body[128] = {0};
static char  g_h3_received_auth[128] = {0};
static int   g_h3_drain_callback_called = 0;
static void* g_h3_drain_callback_data = NULL;

static void
_test_h3_order_handler(csilk_ctx_t* c)
{
    g_h3_handler_invoked = 1;
    const char* auth = csilk_get_header(c, "Authorization");
    if (auth) {
        snprintf(g_h3_received_auth, sizeof(g_h3_received_auth), "%s", auth);
    }
    if (c->request.body && c->request.body_len > 0) {
        snprintf(g_h3_received_body,
                 sizeof(g_h3_received_body),
                 "%.*s",
                 (int)(c->request.body_len < sizeof(g_h3_received_body) - 1
                           ? c->request.body_len
                           : sizeof(g_h3_received_body) - 1),
                 (const char*)c->request.body);
    }
    csilk_set_header(c, "Content-Type", "application/json");
    csilk_string(c, 201, "{\"status\":\"order_created\"}");
}

static void
_test_h3_drain_cb(csilk_ctx_t* c, void* user_data)
{
    (void)c;
    g_h3_drain_callback_called++;
    g_h3_drain_callback_data = user_data;
}

static void
test_h3_varint()
{
    printf("Testing HTTP/3 RFC 9000 Varint encoding/decoding...\n");

    uint64_t test_vals[] = {0,
                            37,
                            63,
                            64,
                            15293,
                            16383,
                            16384,
                            4948729,
                            1073741823ULL,
                            1073741824ULL,
                            4611686018427387903ULL};
    size_t   count = sizeof(test_vals) / sizeof(test_vals[0]);

    for (size_t i = 0; i < count; i++) {
        uint8_t buf[16] = {0};
        size_t  enc_bytes = csilk_h3_varint_encode(test_vals[i], buf);
        assert(enc_bytes > 0);

        uint64_t decoded = 0;
        size_t   dec_bytes = csilk_h3_varint_decode(buf, sizeof(buf), &decoded);
        assert(dec_bytes == enc_bytes);
        assert(decoded == test_vals[i]);
    }

    printf("test_h3_varint: PASS\n");
}

static void
test_h3_varint_edge_cases(void)
{
    printf("Testing HTTP/3 Varint edge cases and truncation...\n");

    /* Null buffer */
    assert(csilk_h3_varint_encode(10, nullptr) == 0);
    assert(csilk_h3_varint_decode(nullptr, 10, nullptr) == 0);

    /* Value exceeds 62 bits */
    uint8_t  buf[16] = {0};
    uint64_t huge_val = (1ULL << 62);
    assert(csilk_h3_varint_encode(huge_val, buf) == 0);

    /* Zero length decode */
    uint64_t decoded = 0;
    assert(csilk_h3_varint_decode(buf, 0, &decoded) == 0);

    /* Truncated multi-byte varints */
    /* 2-byte varint needs 2 bytes, provide only 1 */
    buf[0] = 0x40 | 0x01;
    assert(csilk_h3_varint_decode(buf, 1, &decoded) == 0);

    /* 4-byte varint needs 4 bytes, provide only 2 */
    buf[0] = 0x80 | 0x01;
    assert(csilk_h3_varint_decode(buf, 2, &decoded) == 0);

    /* 8-byte varint needs 8 bytes, provide only 4 */
    buf[0] = 0xC0 | 0x01;
    assert(csilk_h3_varint_decode(buf, 4, &decoded) == 0);

    printf("test_h3_varint_edge_cases: PASS\n");
}

static void
test_h3_frame_edge_cases(void)
{
    printf("Testing HTTP/3 Frame buffer limits and malformed decodes...\n");

    uint8_t out[32];
    /* Buffer too small */
    assert(csilk_h3_frame_encode(CSILK_H3_FRAME_DATA, (const uint8_t*)"1234567890", 10, out, 5) ==
           0);
    assert(csilk_h3_frame_encode(CSILK_H3_FRAME_DATA, nullptr, 0, nullptr, 100) == 0);

    /* Frame decode with null parameters */
    uint64_t type = 0;
    size_t   offset = 0;
    size_t   len = 0;
    assert(csilk_h3_frame_decode(nullptr, 10, &type, &offset, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, nullptr, &offset, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, &type, nullptr, &len) == -1);
    assert(csilk_h3_frame_decode(out, 10, &type, &offset, nullptr) == -1);

    /* Incomplete / truncated payload */
    size_t valid_len = csilk_h3_frame_encode(
        CSILK_H3_FRAME_HEADERS, (const uint8_t*)"header", 6, out, sizeof(out));
    assert(valid_len > 0);
    /* Cut off 2 bytes from payload */
    assert(csilk_h3_frame_decode(out, valid_len - 2, &type, &offset, &len) == -1);

    /* Empty frame decode */
    assert(csilk_h3_frame_decode(out, 0, &type, &offset, &len) == -1);

    printf("test_h3_frame_edge_cases: PASS\n");
}

static void
test_h3_alt_svc_and_listener_edge_cases(void)
{
    printf("Testing Alt-Svc invalid args and listener edge cases...\n");

    /* Alt-Svc null context and invalid ports */
    csilk_h3_inject_alt_svc_header(nullptr, 443);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_h3_inject_alt_svc_header(ctx, 0);
    const char* alt_svc = csilk_get_response_header(ctx, "Alt-Svc");
    assert(alt_svc != nullptr && strstr(alt_svc, "h3=\":443\"") != nullptr);
    csilk_test_ctx_free(ctx);

    /* Listener invalid binds */
    assert(csilk_h3_listener_bind(0) == nullptr);
    assert(csilk_h3_listener_bind(-1) == nullptr);
    assert(csilk_h3_listener_bind(80000) == nullptr);

    /* Listener null close */
    csilk_h3_listener_close(nullptr);

    /* Listener process packet invalid args */
    uint64_t conn_id = 0;
    uint8_t  pkt[16] = {0xC0, 1, 2, 3, 4, 5, 6, 7, 8};
    assert(csilk_h3_listener_process_packet(nullptr, pkt, 16, &conn_id) == -1);

#if CSILK_ENABLE_EXPERIMENTAL_STUBS
    csilk_h3_listener_t* l = csilk_h3_listener_bind(9443);
    assert(l != nullptr);
    assert(csilk_h3_listener_process_packet(l, nullptr, 16, &conn_id) == -1);
    assert(csilk_h3_listener_process_packet(l, pkt, 5, &conn_id) == -1); /* too short */
    assert(csilk_h3_listener_process_packet(l, pkt, 16, nullptr) == -1);
    csilk_h3_listener_close(l);
#endif

    printf("test_h3_alt_svc_and_listener_edge_cases: PASS\n");
}

static void
test_h3_frame_encode_decode()
{
    printf("Testing HTTP/3 Frame encoding/decoding...\n");

    const uint8_t payload[] = {'H', 'T', 'T', 'P', '/', '3', ' ', 'D', 'a', 't', 'a'};
    uint8_t       frame_buf[64] = {0};

    size_t frame_len = csilk_h3_frame_encode(
        CSILK_H3_FRAME_DATA, payload, sizeof(payload), frame_buf, sizeof(frame_buf));
    assert(frame_len > sizeof(payload));

    uint64_t decoded_type = 0ULL;
    size_t   payload_offset = 0;
    size_t   payload_len = 0;

    int res =
        csilk_h3_frame_decode(frame_buf, frame_len, &decoded_type, &payload_offset, &payload_len);
    assert(res == 0);
    assert(decoded_type == CSILK_H3_FRAME_DATA);
    assert(payload_len == sizeof(payload));
    assert(memcmp(frame_buf + payload_offset, payload, sizeof(payload)) == 0);

    printf("test_h3_frame_encode_decode: PASS\n");
}

static void
test_h3_alt_svc_header()
{
    printf("Testing HTTP/3 Alt-Svc header injection...\n");

    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_h3_inject_alt_svc_header(ctx, 443);
    const char* alt_svc = csilk_get_response_header(ctx, "Alt-Svc");
    assert(alt_svc != NULL);
    assert(strstr(alt_svc, "h3=\":443\"") != NULL);

    csilk_test_ctx_free(ctx);
    printf("test_h3_alt_svc_header: PASS\n");
}

static void
test_h3_listener()
{
    printf("Testing HTTP/3 UDP Socket Listener & Connection ID extraction...\n");

    csilk_h3_listener_t* listener = csilk_h3_listener_bind(4433);
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    assert(listener == NULL);
    assert(errno == ENOTSUP);
    printf("test_h3_listener (fail-closed): PASS\n");
    return;
#else
    assert(listener != NULL);

    /* Construct mock QUIC packet header with 64-bit Connection ID: 0x1122334455667788 */
    uint8_t  pkt[12] = {0xC0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x00, 0x00};
    uint64_t conn_id = 0;

    int res = csilk_h3_listener_process_packet(listener, pkt, sizeof(pkt), &conn_id);
    assert(res == 0);
    assert(conn_id == 0x1122334455667788ULL);

    csilk_h3_listener_close(listener);
    printf("test_h3_listener: PASS\n");
#endif
}

static void
test_h3_transport_new_destroy()
{
    printf("Testing HTTP/3 transport new/destroy...\n");

    /* Null args */
    csilk_h3_transport_t* t = csilk_h3_transport_new(0, NULL, NULL);
    assert(t == NULL);

    /* Invalid port */
    t = csilk_h3_transport_new(-1, NULL, NULL);
    assert(t == NULL);

    /* Valid port (binds UDP socket) */
    t = csilk_h3_transport_new(4433, NULL, NULL);
    assert(t != NULL);

    /* Close/destroy */
    assert(csilk_h3_transport_close_conn(t, NULL) == -1);
    assert(csilk_h3_transport_close_stream(t, NULL, 0) == -1);
    csilk_h3_transport_destroy(t);

    /* Null transport */
    csilk_h3_transport_destroy(NULL);

    printf("test_h3_transport_new_destroy: PASS\n");
}

static void
test_h3_session_data()
{
    printf("Testing HTTP/3 stream data alloc/free...\n");

    csilk_h3_stream_data_t* sd = csilk_h3_stream_data_new(42);
    assert(sd != NULL);
    csilk_h3_stream_data_free(sd);
    csilk_h3_stream_data_free(NULL); /* null-safe */

    printf("test_h3_session_data: PASS\n");
}

static void
test_h3_set_enabled()
{
    printf("Testing csilk_h3_set_enabled...\n");

    /* Null server */
    assert(csilk_h3_set_enabled(NULL, 443) == -1);
    /* Invalid port */
    assert(csilk_h3_set_enabled(NULL, 0) == -1);

    printf("test_h3_set_enabled: PASS\n");
}

/* Phase 1.4: HTTP/3 shares router/middleware with HTTP/1.1+2 */
static void
test_h3_dispatch_guard()
{
    printf("Testing csilk_h3_dispatch_stream guard conditions...\n");

    /* NULL client → -1 */
    csilk_h3_stream_data_t* sd = csilk_h3_stream_data_new(0);
    assert(sd);
    sd->headers_complete = 1;
    snprintf(sd->method, sizeof(sd->method), "GET");
    snprintf(sd->path, sizeof(sd->path), "/test");
    assert(csilk_h3_dispatch_stream(NULL, sd) == -1);
    csilk_h3_stream_data_free(sd);

    /* NULL stream data → -1 */
    assert(csilk_h3_dispatch_stream(NULL, NULL) == -1);

    /* Incomplete stream (headers_complete == 0) → -1 */
    csilk_h3_stream_data_t* incomplete = csilk_h3_stream_data_new(0);
    assert(incomplete);
    assert(incomplete->headers_complete == 0); /* not set yet */
    assert(csilk_h3_dispatch_stream(NULL, incomplete) == -1);
    csilk_h3_stream_data_free(incomplete);

    printf("test_h3_dispatch_guard: PASS\n");
}

static void
test_h3_router_dispatch(void)
{
    printf("Testing HTTP/3 unified router dispatch & response lifecycle...\n");

    g_h3_handler_invoked = 0;
    g_h3_received_body[0] = '\0';
    g_h3_received_auth[0] = '\0';

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_h3_order_handler};
    csilk_router_add(router, "POST", "/api/v1/h3_order", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP3;
    client.ctx._internal_client = &client;
    client.ctx.server = server;
    client.ctx.arena = csilk_arena_new(4096);
    client.ctx.file_fd = -1;
    client.ctx.handler_index = -1;
    client.ctx.request_seq = 1;
    client.ctx.write_high_water_mark = CSILK_WRITE_HWM_DEFAULT;
    client.ctx.write_low_water_mark = CSILK_WRITE_LWM_DEFAULT;
    client.ctx.max_write_buffer_size = CSILK_WRITE_MAX_BUFFER_DEFAULT;

    csilk_h3_stream_data_t* sd = csilk_h3_stream_data_new(10);
    assert(sd != NULL);
    snprintf(sd->method, sizeof(sd->method), "POST");
    snprintf(sd->path, sizeof(sd->path), "/api/v1/h3_order");

    sd->num_headers = 2;
    sd->header_names = calloc(64, sizeof(char*));
    sd->header_values = calloc(64, sizeof(char*));
    sd->header_names[0] = malloc(16);
    strcpy(sd->header_names[0], "Content-Type");
    sd->header_values[0] = malloc(20);
    strcpy(sd->header_values[0], "application/json");
    sd->header_names[1] = malloc(16);
    strcpy(sd->header_names[1], "Authorization");
    sd->header_values[1] = malloc(32);
    strcpy(sd->header_values[1], "Bearer quic_token_xyz");

    const char* req_body = "{\"item\":\"item_42\",\"qty\":3}";
    sd->body_buf = malloc(strlen(req_body) + 1);
    strcpy(sd->body_buf, req_body);
    sd->body_len = (uint32_t)strlen(req_body);
    sd->headers_complete = 1;

    int rc = csilk_h3_dispatch_stream(&client, sd);
    assert(rc == 0);
    assert(g_h3_handler_invoked == 1);
    assert(strcmp(g_h3_received_body, req_body) == 0);
    assert(strcmp(g_h3_received_auth, "Bearer quic_token_xyz") == 0);
    assert(client.ctx.response.status == 201);
    assert(client.ctx.response.body != NULL);
    assert(strstr(client.ctx.response.body, "order_created") != NULL);

    csilk_h3_stream_data_free(sd);
    csilk_ctx_cleanup(&client.ctx);
    if (client.ctx.arena) {
        csilk_arena_free(client.ctx.arena);
    }
    csilk_server_free(server);
    csilk_router_free(router);

    printf("test_h3_router_dispatch: PASS\n");
}

#ifdef CSILK_HAS_NGTCP2

/* Phase 1.3: TLS 1.3 + QUIC integration */
static void
test_h3_quic_tls()
{
    printf("Testing csilk_h3_quic_tls_new (TLS 1.3 + QUIC)...\n");

    /* Create an SSL_CTX for testing */
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    assert(ctx);

    /* Create QUIC TLS object */
    SSL* ssl = csilk_h3_quic_tls_new(ctx);
    assert(ssl);

    SSL_free(ssl);
    SSL_CTX_free(ctx);

    /* NULL context → NULL */
    assert(csilk_h3_quic_tls_new(NULL) == NULL);

    printf("test_h3_quic_tls: PASS\n");
}

static void
test_h3_session_send_response_and_body(void)
{
    printf("Testing HTTP/3 session response and body streaming...\n");

    /* 1. Create server session */
    csilk_h3_session_t* s = csilk_h3_session_new(0, NULL);
    assert(s != NULL);

    /* 2. Create client session to feed a valid request on stream 0 */
    nghttp3_conn*     cl = NULL;
    nghttp3_callbacks cl_cb = {0};
    nghttp3_settings  cl_st;
    nghttp3_settings_default(&cl_st);
    int rc = nghttp3_conn_client_new(&cl, &cl_cb, &cl_st, NULL, NULL);
    assert(rc == 0);
    assert(cl != NULL);

    nghttp3_conn_bind_control_stream(cl, 2);
    nghttp3_conn_bind_qpack_streams(cl, 6, 10);

    nghttp3_nv req_nv[] = {
        {(uint8_t*)":method",    (uint8_t*)"GET",       7,  3, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",     7,  5, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/",         5,  1, NGHTTP3_NV_FLAG_NONE},
    };
    rc = nghttp3_conn_submit_request(cl, 0, req_nv, 4, NULL, NULL);
    assert(rc == 0);

    /* Pump generated client frames to server session */
    int64_t     stream_id = -1;
    int         fin = 0;
    nghttp3_vec vec[16];
    while (1) {
        nghttp3_ssize nvec = nghttp3_conn_writev_stream(cl, &stream_id, &fin, vec, 16);
        if (nvec <= 0) {
            break;
        }
        size_t total_written = 0;
        for (nghttp3_ssize i = 0; i < nvec; i++) {
            nghttp3_conn_read_stream(s, stream_id, vec[i].base, vec[i].len, fin);
            total_written += vec[i].len;
        }
        nghttp3_conn_add_write_offset(cl, stream_id, total_written);
    }

    /* 3. Server responds on stream 0 */
    nghttp3_nv hdr;
    hdr.name = (const uint8_t*)"content-type";
    hdr.namelen = 12;
    hdr.value = (const uint8_t*)"text/plain";
    hdr.valuelen = 10;
    hdr.flags = NGHTTP3_NV_FLAG_NONE;

    rc = csilk_h3_session_send_response(s, 0, 200, &hdr, 1, (const uint8_t*)"Hello H3", 8);
    assert(rc == 0);

    rc = csilk_h3_session_send_body(s, 0, (const uint8_t*)" additional chunk", 17);
    assert(rc == 0);

    /* Null guards */
    assert(csilk_h3_session_send_response(NULL, 0, 200, NULL, 0, NULL, 0) == -1);
    assert(csilk_h3_session_send_body(NULL, 0, (const uint8_t*)"x", 1) == -1);
    assert(csilk_h3_session_send_body(s, 0, NULL, 0) == -1);

    nghttp3_conn_del(cl);
    csilk_h3_session_destroy(s);
    csilk_h3_session_destroy(NULL); /* null-safe */

    printf("test_h3_session_send_response_and_body: PASS\n");
}

static void
test_h3_stream_mux(void)
{
    printf("Testing HTTP/3 RFC 9114 concurrent stream multiplexing...\n");

    struct csilk_h3_conn_s conn;
    memset(&conn, 0, sizeof(conn));

    /* Stream map management */
    struct csilk_h3_stream_s* s0 = _h3_get_or_create_stream(&conn, 0);
    struct csilk_h3_stream_s* s4 = _h3_get_or_create_stream(&conn, 4);
    struct csilk_h3_stream_s* s8 = _h3_get_or_create_stream(&conn, 8);

    assert(s0 != NULL && s0->stream_id == 0);
    assert(s4 != NULL && s4->stream_id == 4);
    assert(s8 != NULL && s8->stream_id == 8);
    assert(_h3_get_stream(&conn, 0) == s0);
    assert(_h3_get_stream(&conn, 4) == s4);
    assert(_h3_get_stream(&conn, 8) == s8);
    assert(_h3_get_stream(&conn, 12) == NULL);

    _h3_remove_stream(&conn, 4);
    assert(_h3_get_stream(&conn, 4) == NULL);
    assert(_h3_get_stream(&conn, 0) == s0);
    assert(_h3_get_stream(&conn, 8) == s8);

    /* Router dispatch with concurrent stream contexts */
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_h3_order_handler};
    csilk_router_add(router, "POST", "/api/v1/h3_order", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP3;
    client.h3_conn = &conn;
    conn.client = &client;

    /* Concurrent stream 1: stream_id = 0 */
    csilk_h3_stream_data_t* sd0 = csilk_h3_stream_data_new(0);
    assert(sd0 != NULL);
    csilk_ctx_t* ctx0 = calloc(1, sizeof(csilk_ctx_t));
    assert(ctx0 != NULL);
    ctx0->arena = csilk_arena_new(4096);
    _csilk_stream_ctx_init(ctx0, &client, 0);
    sd0->stream_ctx = ctx0;
    sd0->stream_ctx_owned = 1;
    snprintf(sd0->method, sizeof(sd0->method), "POST");
    snprintf(sd0->path, sizeof(sd0->path), "/api/v1/h3_order");
    const char* b0 = "{\"stream\":0}";
    sd0->body_buf = strdup(b0);
    sd0->body_len = (uint32_t)strlen(b0);
    sd0->headers_complete = 1;

    /* Concurrent stream 2: stream_id = 4 */
    csilk_h3_stream_data_t* sd4 = csilk_h3_stream_data_new(4);
    assert(sd4 != NULL);
    csilk_ctx_t* ctx4 = calloc(1, sizeof(csilk_ctx_t));
    assert(ctx4 != NULL);
    ctx4->arena = csilk_arena_new(4096);
    _csilk_stream_ctx_init(ctx4, &client, 4);
    sd4->stream_ctx = ctx4;
    sd4->stream_ctx_owned = 1;
    snprintf(sd4->method, sizeof(sd4->method), "POST");
    snprintf(sd4->path, sizeof(sd4->path), "/api/v1/h3_order");
    const char* b4 = "{\"stream\":4}";
    sd4->body_buf = strdup(b4);
    sd4->body_len = (uint32_t)strlen(b4);
    sd4->headers_complete = 1;

    /* Dispatch both concurrent streams */
    int rc0 = csilk_h3_dispatch_stream(&client, sd0);
    assert(rc0 == 0);
    assert(ctx0->response.status == 201);
    assert(ctx0->stream_id == 0);

    int rc4 = csilk_h3_dispatch_stream(&client, sd4);
    assert(rc4 == 0);
    assert(ctx4->response.status == 201);
    assert(ctx4->stream_id == 4);

    /* Verify stream contexts are independent and did not clobber each other */
    assert(ctx0 != ctx4);
    assert(ctx0->stream_id == 0);
    assert(ctx4->stream_id == 4);

    /* Destroying stream 0 does not invalidate stream 4 */
    csilk_h3_stream_data_free(sd0);
    assert(ctx4->response.status == 201);

    csilk_h3_stream_data_free(sd4);

    csilk_server_free(server);
    csilk_router_free(router);

    printf("test_h3_stream_mux: PASS\n");
}

static void
test_h3_backpressure_null_guards(void)
{
    printf("Testing HTTP/3 response streaming and backpressure null guards...\n");

    assert(csilk_h3_response_write(NULL, NULL, 0) == -1);
    csilk_h3_response_end(NULL);
    assert(csilk_h3_response_get_write_queue_size(NULL) == 0);
    assert(csilk_h3_response_is_writable(NULL) == 0);
    assert(_h3_get_stream(NULL, 0) == NULL);
    assert(_h3_get_or_create_stream(NULL, 0) == NULL);
    _h3_remove_stream(NULL, 0);

    printf("test_h3_backpressure_null_guards: PASS\n");
}

#ifdef CSILK_HAS_NGTCP2
static void
test_h3_response_streaming_and_backpressure(void)
{
    printf("Testing HTTP/3 response streaming and backpressure...\n");

    g_h3_drain_callback_called = 0;
    g_h3_drain_callback_data = NULL;

    struct csilk_h3_conn_s conn;
    memset(&conn, 0, sizeof(conn));

    /* 1. Create server session with user_data = &conn */
    csilk_h3_session_t* s = csilk_h3_session_new(0, &conn);
    assert(s != NULL);
    conn.nghttp3_conn = s;

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.protocol = CSILK_PROTO_HTTP3;
    client.h3_conn = &conn;
    conn.client = &client;

    /* 2. Create client session to feed a request on stream 0 */
    nghttp3_conn*     cl = NULL;
    nghttp3_callbacks cl_cb = {0};
    nghttp3_settings  cl_st;
    nghttp3_settings_default(&cl_st);
    int rc = nghttp3_conn_client_new(&cl, &cl_cb, &cl_st, NULL, NULL);
    assert(rc == 0);
    assert(cl != NULL);

    nghttp3_conn_bind_control_stream(cl, 2);
    nghttp3_conn_bind_qpack_streams(cl, 6, 10);

    nghttp3_nv req_nv[] = {
        {(uint8_t*)":method",    (uint8_t*)"GET",       7,  3, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":scheme",    (uint8_t*)"https",     7,  5, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9, NGHTTP3_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)"/stream",   5,  7, NGHTTP3_NV_FLAG_NONE},
    };
    rc = nghttp3_conn_submit_request(cl, 0, req_nv, 4, NULL, NULL);
    assert(rc == 0);

    /* Pump client frames to server session */
    int64_t     stream_id = -1;
    int         fin = 0;
    nghttp3_vec vec[16];
    while (1) {
        nghttp3_ssize nvec = nghttp3_conn_writev_stream(cl, &stream_id, &fin, vec, 16);
        if (nvec <= 0) {
            break;
        }
        size_t total_written = 0;
        for (nghttp3_ssize i = 0; i < nvec; i++) {
            nghttp3_conn_read_stream(s, stream_id, vec[i].base, vec[i].len, fin);
            total_written += vec[i].len;
        }
        nghttp3_conn_add_write_offset(cl, stream_id, total_written);
    }

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)nghttp3_conn_get_stream_user_data(s, 0);
    assert(sd != NULL);
    assert(sd->stream_ctx != NULL);
    csilk_ctx_t* ctx = sd->stream_ctx;

    /* Configure backpressure: High=100 bytes, Low=40 bytes, Max=500 bytes */
    csilk_response_set_watermarks(ctx, 100, 40, 500);
    int custom_data = 12345;
    csilk_response_on_drain(ctx, _test_h3_drain_cb, &custom_data);

    assert(csilk_response_is_writable(ctx) == 1);
    assert(csilk_response_get_write_queue_size(ctx) == 0);

    /* Write 50 bytes (below 100 HWM) */
    uint8_t chunk1[50];
    memset(chunk1, 'X', sizeof(chunk1));
    rc = csilk_response_write(ctx, chunk1, sizeof(chunk1));
    assert(rc == 1); /* writable */
    assert(ctx->write_paused == 0);
    assert(csilk_response_is_writable(ctx) == 1);
    assert(csilk_response_get_write_queue_size(ctx) == 50);

    /* Write 60 bytes (total 110 >= 100 HWM -> backpressure triggered!) */
    uint8_t chunk2[60];
    memset(chunk2, 'Y', sizeof(chunk2));
    rc = csilk_response_write(ctx, chunk2, sizeof(chunk2));
    assert(rc == 0); /* backpressure active! */
    assert(ctx->write_paused == 1);
    assert(csilk_response_is_writable(ctx) == 0);
    assert(csilk_response_get_write_queue_size(ctx) == 110);
    assert(g_h3_drain_callback_called == 0);

    /* Attempt to write 400 bytes (110 + 400 = 510 > 500 max limit) -> rejected */
    uint8_t big_chunk[400];
    memset(big_chunk, 'Z', sizeof(big_chunk));
    rc = csilk_response_write(ctx, big_chunk, sizeof(big_chunk));
    assert(rc == -1); /* rejected by max_write_buffer_size */
    assert(csilk_response_get_write_queue_size(ctx) == 110);

    /* Consume frames on server session via writev_stream */
    while (1) {
        nghttp3_ssize nvec = nghttp3_conn_writev_stream(s, &stream_id, &fin, vec, 16);
        if (nvec <= 0) {
            break;
        }
        size_t total_written = 0;
        for (nghttp3_ssize i = 0; i < nvec; i++) {
            total_written += vec[i].len;
        }
        nghttp3_conn_add_write_offset(s, stream_id, total_written);
    }

    /* Simulate peer ACK: remote ACKed 80 bytes of QUIC stream 0 (8 bytes framing + 72 bytes payload) */
    rc = nghttp3_conn_add_ack_offset(s, 0, 80);
    assert(rc == 0);

    /* Queue drops to 38 (<= 40 Low Water Mark): drain callback must trigger */
    assert(csilk_response_get_write_queue_size(ctx) == 38);
    assert(ctx->write_paused == 0);
    assert(csilk_response_is_writable(ctx) == 1);
    assert(g_h3_drain_callback_called == 1);
    assert(g_h3_drain_callback_data == &custom_data);

    /* End the streaming response */
    csilk_response_end(ctx);
    assert(sd->resp_ended == 1);

    nghttp3_conn_del(cl);
    csilk_h3_session_destroy(s);

    printf("test_h3_response_streaming_and_backpressure: PASS\n");
}
#endif

static void
test_h3_0rtt_early_data(void)
{
    printf("Testing HTTP/3 0-RTT early data configuration...\n");

    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    assert(ctx != NULL);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_max_early_data(ctx, 0xffffffff);
    assert(SSL_CTX_get_max_early_data(ctx) == 0xffffffff);

    SSL* ssl = csilk_h3_quic_tls_new(ctx);
    assert(ssl != NULL);
    assert(SSL_get_max_early_data(ssl) == 0xffffffff);

    SSL_free(ssl);
    SSL_CTX_free(ctx);

    printf("test_h3_0rtt_early_data: PASS\n");
}
#endif /* CSILK_HAS_NGTCP2 */

int
main()
{
    test_h3_varint();
    test_h3_varint_edge_cases();
    test_h3_frame_encode_decode();
    test_h3_frame_edge_cases();
    test_h3_alt_svc_header();
    test_h3_alt_svc_and_listener_edge_cases();
    test_h3_listener();
    test_h3_transport_new_destroy();
    test_h3_session_data();
    test_h3_set_enabled();
    test_h3_dispatch_guard();
    test_h3_router_dispatch();
    test_h3_stream_mux();
    test_h3_backpressure_null_guards();
#ifdef CSILK_HAS_NGTCP2
    test_h3_quic_tls();
    test_h3_session_send_response_and_body();
    test_h3_response_streaming_and_backpressure();
    test_h3_0rtt_early_data();
#endif
    printf("All HTTP/3 protocol tests passed successfully!\n");
    return 0;
}
