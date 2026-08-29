/**
 * @file test_h2_dispatch_lifecycle.c
 * @brief Unit tests for HTTP/2 request lifecycle, pseudo-header parsing, trailing headers, and exactly-once dispatch.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/http/h2.h"
#include "csilk/test/test.h"
#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include <nghttp2/nghttp2.h>

/* Declaration of internal nghttp2 callbacks */
int on_header_callback(nghttp2_session*     session,
                       const nghttp2_frame* frame,
                       const uint8_t*       name,
                       size_t               namelen,
                       const uint8_t*       value,
                       size_t               valuelen,
                       uint8_t              flags,
                       void*                user_data);

int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);

int on_data_chunk_recv_callback(nghttp2_session* session,
                                uint8_t          flags,
                                int32_t          stream_id,
                                const uint8_t*   data,
                                size_t           len,
                                void*            user_data);

int on_stream_close_callback(nghttp2_session* session,
                             int32_t          stream_id,
                             uint32_t         error_code,
                             void*            user_data);

static int g_dispatched_count = 0;

static void
_test_handler(csilk_ctx_t* c)
{
    g_dispatched_count++;
    csilk_string(c, 200, "OK");
}

/* -------------------------------------------------------------------------- */
/* Test 1: HEADERS + END_STREAM (No Body)                                     */
/* -------------------------------------------------------------------------- */
static void
test_h2_headers_end_stream(void)
{
    printf("Testing HEADERS + END_STREAM request...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_handler};
    csilk_router_add(router, "POST", "/api/v1/resource", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP2;

    nghttp2_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.hd.type = NGHTTP2_HEADERS;
    frame.hd.stream_id = 1;
    frame.headers.cat = NGHTTP2_HCAT_REQUEST;

    /* Feed headers */
    const char* m_name = ":method";
    const char* m_val = "POST";
    on_header_callback(NULL,
                       &frame,
                       (const uint8_t*)m_name,
                       strlen(m_name),
                       (const uint8_t*)m_val,
                       strlen(m_val),
                       0,
                       &client);

    const char* p_name = ":path";
    const char* p_val = "/api/v1/resource?query=1";
    on_header_callback(NULL,
                       &frame,
                       (const uint8_t*)p_name,
                       strlen(p_name),
                       (const uint8_t*)p_val,
                       strlen(p_val),
                       0,
                       &client);

    const char* a_name = ":authority";
    const char* a_val = "example.com:8443";
    on_header_callback(NULL,
                       &frame,
                       (const uint8_t*)a_name,
                       strlen(a_name),
                       (const uint8_t*)a_val,
                       strlen(a_val),
                       0,
                       &client);

    const char* c_name = "x-custom-test";
    const char* c_val = "custom_value_42";
    on_header_callback(NULL,
                       &frame,
                       (const uint8_t*)c_name,
                       strlen(c_name),
                       (const uint8_t*)c_val,
                       strlen(c_val),
                       0,
                       &client);

    csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, 1);
    assert(c != NULL);
    assert(c->headers_received == 1);
    assert(c->request_dispatched == 0);
    assert(strcmp(c->request.method, "POST") == 0);
    assert(strcmp(c->request.path, "/api/v1/resource") == 0);
    assert(strcmp(csilk_get_query(c, "query"), "1") == 0);
    assert(strcmp(csilk_get_header(c, "Host"), "example.com:8443") == 0);
    assert(strcmp(csilk_get_header(c, "x-custom-test"), "custom_value_42") == 0);

    /* End stream */
    g_dispatched_count = 0;
    frame.hd.flags |= NGHTTP2_FLAG_END_STREAM;
    on_frame_recv_callback(NULL, &frame, &client);

    assert(c->end_stream_received == 1);
    assert(c->request_dispatched == 1);
    assert(g_dispatched_count == 1);

    /* Duplicate END_STREAM frame must NOT dispatch again */
    on_frame_recv_callback(NULL, &frame, &client);
    assert(g_dispatched_count == 1);

    csilk_h2_free_streams(&client);
    csilk_server_free(server);
    printf("test_h2_headers_end_stream: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: HEADERS + DATA chunks + END_STREAM                                 */
/* -------------------------------------------------------------------------- */
static void
test_h2_headers_data_end_stream(void)
{
    printf("Testing HEADERS + DATA + END_STREAM request...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_handler};
    csilk_router_add(router, "POST", "/upload", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP2;

    nghttp2_frame h_frame;
    memset(&h_frame, 0, sizeof(h_frame));
    h_frame.hd.type = NGHTTP2_HEADERS;
    h_frame.hd.stream_id = 3;
    h_frame.headers.cat = NGHTTP2_HCAT_REQUEST;

    const char* m_name = ":method";
    const char* m_val = "POST";
    on_header_callback(NULL,
                       &h_frame,
                       (const uint8_t*)m_name,
                       strlen(m_name),
                       (const uint8_t*)m_val,
                       strlen(m_val),
                       0,
                       &client);

    const char* p_name = ":path";
    const char* p_val = "/upload";
    on_header_callback(NULL,
                       &h_frame,
                       (const uint8_t*)p_name,
                       strlen(p_name),
                       (const uint8_t*)p_val,
                       strlen(p_val),
                       0,
                       &client);

    /* Chunk 1 */
    const char* d1 = "chunk1_";
    on_data_chunk_recv_callback(NULL, 0, 3, (const uint8_t*)d1, strlen(d1), &client);

    nghttp2_frame d1_frame;
    memset(&d1_frame, 0, sizeof(d1_frame));
    d1_frame.hd.type = NGHTTP2_DATA;
    d1_frame.hd.stream_id = 3;
    on_frame_recv_callback(NULL, &d1_frame, &client);

    csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, 3);
    assert(c->request_dispatched == 0);

    /* Chunk 2 with END_STREAM */
    const char* d2 = "chunk2_payload";
    on_data_chunk_recv_callback(NULL, 0, 3, (const uint8_t*)d2, strlen(d2), &client);

    g_dispatched_count = 0;
    d1_frame.hd.flags |= NGHTTP2_FLAG_END_STREAM;
    on_frame_recv_callback(NULL, &d1_frame, &client);

    assert(c->request_dispatched == 1);
    assert(g_dispatched_count == 1);
    assert(strcmp(c->request.body, "chunk1_chunk2_payload") == 0);

    csilk_h2_free_streams(&client);
    csilk_server_free(server);
    printf("test_h2_headers_data_end_stream: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: HEADERS + DATA + Trailing HEADERS (Trailers)                       */
/* -------------------------------------------------------------------------- */
static void
test_h2_headers_data_trailers_end_stream(void)
{
    printf("Testing HEADERS + DATA + Trailing HEADERS (Trailers)...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_handler};
    csilk_router_add(router, "POST", "/trailers", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP2;

    nghttp2_frame h_frame;
    memset(&h_frame, 0, sizeof(h_frame));
    h_frame.hd.type = NGHTTP2_HEADERS;
    h_frame.hd.stream_id = 5;
    h_frame.headers.cat = NGHTTP2_HCAT_REQUEST;

    const char* m_name = ":method";
    const char* m_val = "POST";
    on_header_callback(NULL,
                       &h_frame,
                       (const uint8_t*)m_name,
                       strlen(m_name),
                       (const uint8_t*)m_val,
                       strlen(m_val),
                       0,
                       &client);

    const char* p_name = ":path";
    const char* p_val = "/trailers";
    on_header_callback(NULL,
                       &h_frame,
                       (const uint8_t*)p_name,
                       strlen(p_name),
                       (const uint8_t*)p_val,
                       strlen(p_val),
                       0,
                       &client);

    /* Body */
    const char* d = "stream body data";
    on_data_chunk_recv_callback(NULL, 0, 5, (const uint8_t*)d, strlen(d), &client);

    /* Trailing headers (trailers) */
    nghttp2_frame t_frame;
    memset(&t_frame, 0, sizeof(t_frame));
    t_frame.hd.type = NGHTTP2_HEADERS;
    t_frame.hd.stream_id = 5;
    t_frame.headers.cat = NGHTTP2_HCAT_HEADERS; /* Trailer category */

    const char* t_name = "x-checksum-sha256";
    const char* t_val = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    on_header_callback(NULL,
                       &t_frame,
                       (const uint8_t*)t_name,
                       strlen(t_name),
                       (const uint8_t*)t_val,
                       strlen(t_val),
                       0,
                       &client);

    g_dispatched_count = 0;
    t_frame.hd.flags |= NGHTTP2_FLAG_END_STREAM;
    on_frame_recv_callback(NULL, &t_frame, &client);

    csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, 5);
    assert(c->request_dispatched == 1);
    assert(g_dispatched_count == 1);
    assert(strcmp(csilk_get_header(c, "x-checksum-sha256"), t_val) == 0);

    csilk_h2_free_streams(&client);
    csilk_server_free(server);
    printf("test_h2_headers_data_trailers_end_stream: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: RST_STREAM Before Dispatch                                         */
/* -------------------------------------------------------------------------- */
static void
test_h2_rst_stream_before_dispatch(void)
{
    printf("Testing RST_STREAM before dispatch cancellation...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_handler};
    csilk_router_add(router, "GET", "/reset", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP2;

    nghttp2_frame h_frame;
    memset(&h_frame, 0, sizeof(h_frame));
    h_frame.hd.type = NGHTTP2_HEADERS;
    h_frame.hd.stream_id = 7;
    h_frame.headers.cat = NGHTTP2_HCAT_REQUEST;

    const char* m_name = ":method";
    const char* m_val = "GET";
    on_header_callback(NULL,
                       &h_frame,
                       (const uint8_t*)m_name,
                       strlen(m_name),
                       (const uint8_t*)m_val,
                       strlen(m_val),
                       0,
                       &client);

    /* Stream reset callback */
    g_dispatched_count = 0;
    on_stream_close_callback(NULL, 7, NGHTTP2_CANCEL, &client);

    /* Spurious END_STREAM received after reset */
    h_frame.hd.flags |= NGHTTP2_FLAG_END_STREAM;
    on_frame_recv_callback(NULL, &h_frame, &client);

    /* Must NOT have dispatched */
    assert(g_dispatched_count == 0);

    csilk_h2_free_streams(&client);
    csilk_server_free(server);
    printf("test_h2_rst_stream_before_dispatch: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: 100 Concurrent Streams Exactly-Once Dispatch                       */
/* -------------------------------------------------------------------------- */
static void
test_h2_100_concurrent_streams_dispatch(void)
{
    printf("Testing 100 concurrent HTTP/2 streams exactly-once dispatch...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {_test_handler};
    csilk_router_add(router, "GET", "/test", handlers, 1);

    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.server = server;
    client.protocol = CSILK_PROTO_HTTP2;

    const int NUM_STREAMS = 100;
    g_dispatched_count = 0;

    for (int i = 0; i < NUM_STREAMS; i++) {
        int32_t stream_id = i * 2 + 1;

        nghttp2_frame h_frame;
        memset(&h_frame, 0, sizeof(h_frame));
        h_frame.hd.type = NGHTTP2_HEADERS;
        h_frame.hd.stream_id = stream_id;
        h_frame.headers.cat = NGHTTP2_HCAT_REQUEST;

        const char* m_name = ":method";
        const char* m_val = "GET";
        on_header_callback(NULL,
                           &h_frame,
                           (const uint8_t*)m_name,
                           strlen(m_name),
                           (const uint8_t*)m_val,
                           strlen(m_val),
                           0,
                           &client);

        const char* p_name = ":path";
        const char* p_val = "/test";
        on_header_callback(NULL,
                           &h_frame,
                           (const uint8_t*)p_name,
                           strlen(p_name),
                           (const uint8_t*)p_val,
                           strlen(p_val),
                           0,
                           &client);

        h_frame.hd.flags |= NGHTTP2_FLAG_END_STREAM;
        on_frame_recv_callback(NULL, &h_frame, &client);
    }

    assert(g_dispatched_count == NUM_STREAMS);

    csilk_h2_free_streams(&client);
    csilk_server_free(server);
    printf("test_h2_100_concurrent_streams_dispatch: PASS\n");
}

int
main(void)
{
    printf("=== Running HTTP/2 Dispatch Lifecycle Tests ===\n");
    test_h2_headers_end_stream();
    test_h2_headers_data_end_stream();
    test_h2_headers_data_trailers_end_stream();
    test_h2_rst_stream_before_dispatch();
    test_h2_100_concurrent_streams_dispatch();
    printf("=== All HTTP/2 Dispatch Lifecycle Tests Passed! ===\n");
    return 0;
}
