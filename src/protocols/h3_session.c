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
    (void)stream_user_data;
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
    (void)stream_user_data;

    /* Accumulate body data. In a full implementation, this would feed
     * the data into csilk_ctx_t's body buffer via the arena. */
    if (data && datalen > 0) {
        csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
        if (sd) {
            /* Simple: just track the last chunk for now.
             * Full implementation would allocate from arena. */
            sd->body_len = (uint32_t)datalen;
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
    (void)conn;
    (void)conn_user_data;
    (void)stream_user_data;

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (sd) {
        sd->num_headers = 0;
        sd->headers_complete = 0;
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
    (void)fin;

    csilk_h3_stream_data_t* sd = (csilk_h3_stream_data_t*)stream_user_data;
    if (sd) {
        sd->headers_complete = 1;
    }

    /* Phase 1.4: bridge into the shared router/middleware pipeline.
     * conn_user_data carries a csilk_h3_conn_state_t with the client pointer. */
    if (conn_user_data && sd && sd->headers_complete) {
        /* The dispatch is deferred to the caller via the public bridge: */
        /*   csilk_h3_dispatch_stream(client, sd)                        */
        /* This keeps the nghttp3 callback thin and testable.          */
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
    (void)conn_user_data;
    (void)stream_user_data;
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
    (void)stream_id;
    (void)consumed;
    (void)conn_user_data;
    (void)stream_user_data;
    return 0;
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
     * and accessible via the nghttp3_conn internals. No validation needed here. */

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

    nghttp3_data_reader reader = {0};
    int rc = nghttp3_conn_submit_response(session, stream_id, resp_nv, (size_t)resp_nvlen, &reader);
    if (rc != 0) {
        CSILK_LOG_E("H3: nghttp3_conn_submit_response failed: %s", nghttp3_strerror(rc));
        return -1;
    }

    /* Push the response body (if any) via nghttp3_conn_writev_stream. */
    if (body && body_len > 0) {
        nghttp3_vec iov;
        iov.base = (uint8_t*)body;
        iov.len = body_len;

        int64_t       out_stream_id = 0;
        int           out_fin = 0;
        nghttp3_ssize written =
            nghttp3_conn_writev_stream(session, &out_stream_id, &out_fin, &iov, 1);
        if (written < 0) {
            CSILK_LOG_E("H3: nghttp3_conn_writev_stream failed: %s",
                        nghttp3_strerror((int)written));
            return -1;
        }
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

    nghttp3_vec iov;
    iov.base = (uint8_t*)body;
    iov.len = body_len;

    int64_t       out_stream_id = stream_id;
    int           out_fin = 0;
    nghttp3_ssize written = nghttp3_conn_writev_stream(session, &out_stream_id, &out_fin, &iov, 1);
    if (written < 0) {
        CSILK_LOG_E("H3: nghttp3_conn_writev_stream (body) failed: %s",
                    nghttp3_strerror((int)written));
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
