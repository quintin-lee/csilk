/**
 * @file h3_session.c
 * @brief HTTP/3 per-connection session: stream management, flow control, and
 *        nghttp3 QPACK header compression integration.
 *
 * A csilk_h3_session_t wraps a single QUIC connection and its nghttp3_conn,
 * managing up to 128 concurrent HTTP/3 streams. Each stream maps to a
 * csilk_ctx_t for the router/middleware pipeline, just like HTTP/1.1 and
 * HTTP/2 connections.
 *
 * The nghttp3 callback model is header-by-header: begin_headers →
 * recv_header (×N) → end_headers, then recv_data for body chunks.
 *
 * @copyright MIT License
 */

#include "csilk/protocols/h3.h"
#include "csilk/csilk.h"
#include "http3_internal.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/header_map.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CSILK_HAS_NGTCP2
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#endif

#ifdef CSILK_HAS_NGTCP2

static int
_h3_stream_close_cb(nghttp3_conn* conn,
                    int64_t       stream_id,
                    uint64_t      app_error_code,
                    void*         conn_user_data,
                    void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)app_error_code;
    (void)conn_user_data;
    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (sd) {
        csilk_h3_stream_data_free(sd);
        nghttp3_conn_set_stream_user_data(conn, stream_id, NULL);
    }
    return 0;
}

static int
_h3_recv_data_cb(nghttp3_conn*  conn,
                 int64_t        stream_id,
                 const uint8_t* data,
                 size_t         datalen,
                 void*          conn_user_data,
                 void*          stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)conn_user_data;

    if (data && datalen > 0) {
        csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
        if (sd) {
            /* Bound memory usage per stream to max 10MB */
            if (sd->body_len + datalen <= 10 * 1024 * 1024) {
                char* new_buf = realloc(sd->body_buf, sd->body_len + datalen + 1);
                if (new_buf) {
                    sd->body_buf = new_buf;
                    memcpy(sd->body_buf + sd->body_len, data, datalen);
                    sd->body_len += (uint32_t)datalen;
                    sd->body_buf[sd->body_len] = '\0';
                }
            }
        }
    }
    return 0;
}

static int
_h3_begin_headers_cb(nghttp3_conn* conn,
                     int64_t       stream_id,
                     void*         conn_user_data,
                     void*         stream_user_data)
{
    (void)conn_user_data;
    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (!sd) {
        sd = csilk_h3_stream_data_new((uint64_t)stream_id);
        if (!sd) {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        nghttp3_conn_set_stream_user_data(conn, stream_id, sd);
    } else {
        sd->num_headers = 0;
        sd->headers_complete = 0;
        sd->dispatched = 0;
    }
    return 0;
}

static int
_h3_recv_header_cb(nghttp3_conn*  conn,
                   int64_t        stream_id,
                   int32_t        token,
                   nghttp3_rcbuf* name,
                   nghttp3_rcbuf* value,
                   uint8_t        flags,
                   void*          conn_user_data,
                   void*          stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)flags;
    (void)conn_user_data;

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (!sd || !name || !value) {
        return 0;
    }

    const uint8_t* nbase = nghttp3_rcbuf_get_buf(name).base;
    const uint8_t* vbase = nghttp3_rcbuf_get_buf(value).base;
    size_t         nlen = nghttp3_rcbuf_get_buf(name).len;
    size_t         vlen = nghttp3_rcbuf_get_buf(value).len;

    /* Handle pseudo-headers */
    if (nlen == 7 && memcmp(nbase, ":method", 7) == 0) {
        if (vlen >= sizeof(sd->method)) {
            vlen = sizeof(sd->method) - 1;
        }
        memcpy(sd->method, vbase, vlen);
        sd->method[vlen] = '\0';
    } else if (nlen == 5 && memcmp(nbase, ":path", 5) == 0) {
        if (vlen >= sizeof(sd->path)) {
            vlen = sizeof(sd->path) - 1;
        }
        memcpy(sd->path, vbase, vlen);
        sd->path[vlen] = '\0';
    }

    /* Store in header arrays (bounded allocation) */
    if (sd->num_headers < 64) {
        if (!sd->header_names) {
            sd->header_names = calloc(64, sizeof(char*));
            sd->header_values = calloc(64, sizeof(char*));
        }
        if (sd->header_names && sd->header_values) {
            char* hname = malloc(nlen + 1);
            char* hval = malloc(vlen + 1);
            if (hname && hval) {
                memcpy(hname, nbase, nlen);
                hname[nlen] = '\0';
                memcpy(hval, vbase, vlen);
                hval[vlen] = '\0';
                sd->header_names[sd->num_headers] = hname;
                sd->header_values[sd->num_headers] = hval;
                sd->num_headers++;
            }
        }
    }
    (void)token;
    return 0;
}

static int
_h3_end_headers_cb(
    nghttp3_conn* conn, int64_t stream_id, int fin, void* conn_user_data, void* stream_user_data)
{
    (void)conn;
    (void)stream_id;

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (sd) {
        sd->headers_complete = 1;
    }

    if (fin && conn_user_data && sd && !sd->dispatched) {
        struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)conn_user_data;
        if (c->client) {
            sd->dispatched = 1;
            csilk_h3_dispatch_stream((csilk_client_t*)c->client, sd);
        }
    }
    return 0;
}

static int
_h3_end_stream_cb(nghttp3_conn* conn,
                  int64_t       stream_id,
                  void*         conn_user_data,
                  void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (conn_user_data && sd && sd->headers_complete && !sd->dispatched) {
        struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)conn_user_data;
        if (c->client) {
            sd->dispatched = 1;
            csilk_h3_dispatch_stream((csilk_client_t*)c->client, sd);
        }
    }
    return 0;
}

static int
_h3_stop_sending_cb(nghttp3_conn* conn,
                    int64_t       stream_id,
                    uint64_t      app_error_code,
                    void*         conn_user_data,
                    void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)app_error_code;
    (void)conn_user_data;
    (void)stream_user_data;
    return 0;
}

static int
_h3_reset_stream_cb(nghttp3_conn* conn,
                    int64_t       stream_id,
                    uint64_t      app_error_code,
                    void*         conn_user_data,
                    void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)app_error_code;
    (void)conn_user_data;
    (void)stream_user_data;
    return 0;
}

static int
_h3_acked_stream_data_cb(nghttp3_conn* conn,
                         int64_t       stream_id,
                         uint64_t      datalen,
                         void*         conn_user_data,
                         void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)datalen;
    (void)conn_user_data;
    (void)stream_user_data;
    return 0;
}

static int
_h3_deferred_consume_cb(nghttp3_conn* conn,
                        int64_t       stream_id,
                        size_t        consumed,
                        void*         conn_user_data,
                        void*         stream_user_data)
{
    (void)conn;
    (void)stream_user_data;
    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)conn_user_data;
    if (c && c->ngtcp2_conn && consumed > 0) {
        ngtcp2_conn_extend_max_stream_offset(c->ngtcp2_conn, stream_id, (uint64_t)consumed);
        ngtcp2_conn_extend_max_offset(c->ngtcp2_conn, (uint64_t)consumed);
    }
    return 0;
}

static nghttp3_ssize
_h3_read_data_cb(nghttp3_conn* conn,
                 int64_t       stream_id,
                 nghttp3_vec*  vec,
                 size_t        veccnt,
                 uint32_t*     pflags,
                 void*         conn_user_data,
                 void*         stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)conn_user_data;
    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (!sd || !sd->resp_body || sd->resp_body_offset >= sd->resp_body_len) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    if (veccnt == 0) {
        return 0;
    }
    size_t rem = sd->resp_body_len - sd->resp_body_offset;
    vec[0].base = (uint8_t*)(sd->resp_body + sd->resp_body_offset);
    vec[0].len = rem;
    sd->resp_body_offset += rem;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

#endif /* CSILK_HAS_NGTCP2 */

/* ===================================================================
 * Public session API
 * =================================================================== */

#ifdef CSILK_HAS_NGTCP2

/**
 * @brief Create an HTTP/3 session for a new QUIC connection.
 *
 * @param stream_id   First stream ID (typically 0 for server).
 * @param user_data   Per-connection user data. Passed to nghttp3 as the
 *                    session's user_data, so nghttp3_conn_get_user_data() on
 *                    the returned session returns @p user_data. In production
 *                    this is a csilk_h3_conn_s*.
 * @return Newly allocated nghttp3_conn* session, or NULL on failure.
 */
nghttp3_conn*
csilk_h3_session_new(int64_t stream_id, void* user_data)
{
    (void)stream_id;

    nghttp3_callbacks callbacks = {
        .acked_stream_data = _h3_acked_stream_data_cb,
        .stream_close = _h3_stream_close_cb,
        .recv_data = _h3_recv_data_cb,
        .deferred_consume = _h3_deferred_consume_cb,
        .begin_headers = _h3_begin_headers_cb,
        .recv_header = _h3_recv_header_cb,
        .end_headers = _h3_end_headers_cb,
        .stop_sending = _h3_stop_sending_cb,
        .end_stream = _h3_end_stream_cb,
        .reset_stream = _h3_reset_stream_cb,
    };

    nghttp3_settings settings = {
        .qpack_max_dtable_capacity = 4096,
        .qpack_blocked_streams = 16,
    };

    nghttp3_conn* ng3 = NULL;
    int           rc = nghttp3_conn_server_new_versioned(&ng3,
                                                         NGHTTP3_CALLBACKS_VERSION,
                                                         &callbacks,
                                                         NGHTTP3_SETTINGS_VERSION,
                                                         &settings,
                                                         NULL, /* mem */
                                                         user_data);
    if (rc != 0) {
        CSILK_LOG_E("H3: nghttp3_conn_server_new_versioned failed: %s", nghttp3_strerror(rc));
        return NULL;
    }

    /* The nghttp3 user_data is retrieved via nghttp3_conn_get_stream_user_data
     * (per-stream). The connection-level user_data is set at session creation
     * and accessible via the nghttp3_conn internals. */

    int64_t ctrl_stream_id = 3;
    int64_t qpack_enc_stream_id = 7;
    int64_t qpack_dec_stream_id = 11;

    struct csilk_h3_conn_s* c = (struct csilk_h3_conn_s*)user_data;
    if (c && c->ngtcp2_conn) {
        ngtcp2_conn_open_uni_stream(c->ngtcp2_conn, &ctrl_stream_id, NULL);
        ngtcp2_conn_open_uni_stream(c->ngtcp2_conn, &qpack_enc_stream_id, NULL);
        ngtcp2_conn_open_uni_stream(c->ngtcp2_conn, &qpack_dec_stream_id, NULL);
    }

    nghttp3_conn_bind_control_stream(ng3, ctrl_stream_id);
    nghttp3_conn_bind_qpack_streams(ng3, qpack_enc_stream_id, qpack_dec_stream_id);

    return ng3;
}

/**
 * @brief Destroy an HTTP/3 session.
 *
 * @param session  nghttp3_conn* to free.
 */
void
csilk_h3_session_destroy(nghttp3_conn* session)
{
    if (session) {
        nghttp3_conn_del(session);
    }
}

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
int
csilk_h3_session_send_response(nghttp3_conn*     session,
                               int64_t           stream_id,
                               int               status,
                               const nghttp3_nv* headers,
                               int               nheaders,
                               const uint8_t*    body,
                               size_t            body_len)
{
    if (!session) {
        return -1;
    }

    /* Per-connection scratch buffer (thread-safe: one array per H3 connection).
     * The nghttp3 session was created with user_data = csilk_h3_conn_s*.
     * Since nghttp3 has no public getter for the connection-level user_data,
     * we use a stack-allocated buffer. This is thread-safe because each H3
     * connection is serviced by a single worker thread. */
    nghttp3_nv resp_nv[64];
    int        resp_nv_cap = 64;

    /* Build response headers: :status pseudo-header */
    int resp_nvlen = 0;

    char status_buf[16];
    int  status_len = snprintf(status_buf, sizeof(status_buf), "%d", status);
    resp_nv[resp_nvlen].name = (const uint8_t*)":status";
    resp_nv[resp_nvlen].namelen = 7;
    resp_nv[resp_nvlen].value = (const uint8_t*)status_buf;
    resp_nv[resp_nvlen].valuelen = (size_t)status_len;
    resp_nvlen++;

    /* User-provided headers */
    if (headers && nheaders > 0) {
        int max_copy = (nheaders < resp_nv_cap - 1) ? nheaders : (resp_nv_cap - 1);
        for (int i = 0; i < max_copy; i++) {
            resp_nv[resp_nvlen] = headers[i];
            resp_nvlen++;
        }
    }

    csilk_h3_stream_data_t* sd =
        (csilk_h3_stream_data_t*)nghttp3_conn_get_stream_user_data(session, stream_id);
    if (!sd) {
        sd = csilk_h3_stream_data_new((uint64_t)stream_id);
        if (sd) {
            nghttp3_conn_set_stream_user_data(session, stream_id, sd);
        }
    }
    if (sd) {
        sd->resp_body = body;
        sd->resp_body_len = body_len;
        sd->resp_body_offset = 0;
    }

    nghttp3_data_reader dr = {.read_data = _h3_read_data_cb};
    int                 rc = nghttp3_conn_submit_response(
        session, stream_id, resp_nv, (size_t)resp_nvlen, (body && body_len > 0) ? &dr : NULL);
    if (rc != 0) {
        CSILK_LOG_E("H3: nghttp3_conn_submit_response failed: %s", nghttp3_strerror(rc));
        return -1;
    }

    return 0;
}

/**
 * @brief Push body data on an existing HTTP/3 stream.
 *
 * Intended for streaming responses: after calling csilk_h3_session_send_response
 * with body=NULL, repeatedly call this to emit body chunks. The caller is
 * responsible for sending the resulting QUIC packets (see _h3_send_quic_packets).
 *
 * @param session    HTTP/3 session (nghttp3_conn*).
 * @param stream_id  Target stream ID.
 * @param body       Body chunk to send.
 * @param body_len   Length of the chunk.
 * @return 0 on success, -1 on error.
 */
int
csilk_h3_session_send_body(nghttp3_conn*  session,
                           int64_t        stream_id,
                           const uint8_t* body,
                           size_t         body_len)
{
    if (!session || !body || body_len == 0) {
        return -1;
    }

    csilk_h3_stream_data_t* sd =
        (csilk_h3_stream_data_t*)nghttp3_conn_get_stream_user_data(session, stream_id);
    if (!sd) {
        return -1;
    }

    sd->resp_body = body;
    sd->resp_body_len = body_len;
    sd->resp_body_offset = 0;

    int rc = nghttp3_conn_resume_stream(session, stream_id);
    if (rc != 0) {
        CSILK_LOG_E("H3: nghttp3_conn_resume_stream failed: %s", nghttp3_strerror(rc));
        return -1;
    }
    return 0;
}

#endif /* CSILK_HAS_NGTCP2 */

/**
 * @brief Bridge: dispatch a completed HTTP/3 stream into the shared
 *        router/middleware pipeline (same as HTTP/1.1 and HTTP/2).
 *
 * Populates client->ctx.request.{method, path, headers, body} from
 * the collected csilk_h3_stream_data_t, then invokes
 * _csilk_dispatch_request() which runs the full middleware chain,
 * route match, handler, and response serialization.
 *
 * @param client  The csilk_client_t owning this stream (from the pool).
 * @param sd      The completed stream data (headers_complete == 1).
 * @return 0 on success, -1 if the stream data is incomplete.
 */
int
csilk_h3_dispatch_stream(csilk_client_t* client, csilk_h3_stream_data_t* sd)
{
    if (!client || !sd || !sd->headers_complete) {
        return -1;
    }

    csilk_ctx_t* ctx = &client->ctx;

    client->protocol = CSILK_PROTO_HTTP3;
    ctx->stream_id = (int32_t)sd->stream_id;
    ctx->handler_index = -1;
    ctx->request_seq++;

    /* Method */
    ctx->request.method = csilk_arena_strdup(ctx->arena, sd->method[0] ? sd->method : "GET");
    if (!ctx->request.method) {
        ctx->request.method = "GET";
    }

    /* Path (URL-decode is handled by the router; store raw) */
    ctx->request.path = csilk_arena_strdup(ctx->arena, sd->path[0] ? sd->path : "/");
    if (!ctx->request.path) {
        ctx->request.path = csilk_arena_strdup(ctx->arena, "/");
    }

    /* Request headers */
    for (uint32_t i = 0; i < sd->num_headers; i++) {
        if (sd->header_names[i] && sd->header_values[i]) {
            map_set(ctx, &ctx->request.headers, sd->header_names[i], sd->header_values[i]);
        }
    }

    /* Body */
    if (sd->body_buf && sd->body_len > 0) {
        ctx->request.body = sd->body_buf;
        ctx->request.body_len = sd->body_len;
        ctx->request.body_ownership = CSILK_OWN_BORROWED;
    } else {
        ctx->request.body = NULL;
        ctx->request.body_len = 0;
        ctx->request.body_ownership = CSILK_OWN_NONE;
    }

    /* Dispatch through the shared middleware/router pipeline */
    _csilk_dispatch_request(ctx);

    return 0;
}

/**
 * @brief Submit an HTTP/3 response for a completed request context.
 *
 * @param c The request context.
 */
void
csilk_h3_send_response_for_ctx(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client) {
        return;
    }

#ifdef CSILK_HAS_NGTCP2
    struct csilk_h3_conn_s* h3c = (struct csilk_h3_conn_s*)client->h3_conn;
    if (!h3c || !h3c->nghttp3_conn) {
        return;
    }

    int status = c->response.status ? c->response.status : 200;

    int header_count = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            header_count++;
        }
    }

    nghttp3_nv  stack_nva[64];
    nghttp3_nv* nva = stack_nva;
    if (header_count > 64) {
        nva = malloc(sizeof(nghttp3_nv) * (size_t)header_count);
        if (!nva) {
            return;
        }
    }

    int idx = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS && idx < header_count; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            nva[idx].name = (const uint8_t*)h->key;
            nva[idx].namelen = h->key_len;
            nva[idx].value = (const uint8_t*)h->value;
            nva[idx].valuelen = h->value_len;
            nva[idx].flags = NGHTTP3_NV_FLAG_NONE;
            idx++;
        }
    }

    int rc = csilk_h3_session_send_response(h3c->nghttp3_conn,
                                            c->stream_id,
                                            status,
                                            nva,
                                            idx,
                                            (const uint8_t*)c->response.body,
                                            c->response.body_len);

    if (nva != stack_nva) {
        free(nva);
    }

    if (rc == 0 && h3c->transport) {
        _h3_flush_http3_to_quic(h3c);
        _h3_send_quic_packets(h3c->transport, h3c);
    }

    if (client->server) {
        _csilk_trigger_hooks(client->server, c, CSILK_HOOK_REQUEST_END);
    }
#else
    (void)c;
#endif
}

/* --- HTTP/3 Stream data (always available) --- */

/**
 * @brief Allocate per-stream user data for an HTTP/3 stream.
 *
 * @param stream_id  Stream ID.
 * @return Heap-allocated csilk_h3_stream_data_t, or NULL on OOM.
 */
csilk_h3_stream_data_t*
csilk_h3_stream_data_new(uint64_t stream_id)
{
    csilk_h3_stream_data_t* sd = calloc(1, sizeof(csilk_h3_stream_data_t));
    if (!sd) {
        return NULL;
    }
    sd->stream_id = stream_id;
    return sd;
}

/**
 * @brief Free per-stream user data.
 */
void
csilk_h3_stream_data_free(csilk_h3_stream_data_t* sd)
{
    if (!sd) {
        return;
    }
    for (uint32_t i = 0; i < sd->num_headers; i++) {
        free(sd->header_names[i]);
        free(sd->header_values[i]);
    }
    free(sd->header_names);
    free(sd->header_values);
    free(sd->body_buf);
    free(sd);
}
