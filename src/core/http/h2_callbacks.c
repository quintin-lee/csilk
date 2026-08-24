/**
 * @file h2_callbacks.c
 * @brief nghttp2 callback implementations for HTTP/2 session handling.
 *
 * Split from h2.c. Contains all static nghttp2 callback functions and
 * a registration helper that wires them into an nghttp2_session_callbacks.
 *
 * @copyright MIT License
 */

#include "csilk/http/h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Called by nghttp2 when a HEADERS frame is received and parsing begins. */
int
on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    (void)session;
    (void)frame;
    (void)user_data;
    return 0;
}

/** @brief Called by nghttp2 for each header name/value pair in a HEADERS frame. */
int
on_header_callback(nghttp2_session*     session,
                   const nghttp2_frame* frame,
                   const uint8_t*       name,
                   size_t               namelen,
                   const uint8_t*       value,
                   size_t               valuelen,
                   uint8_t              flags,
                   void*                user_data)
{
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }

    csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
    if (!c) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (name[0] == ':') {
        if (strncmp((const char*)name, ":method", namelen) == 0) {
            c->request.method = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
        } else if (strncmp((const char*)name, ":path", namelen) == 0) {
            char* full_path = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
            char* path;
            char* query;
            csilk_split_url(full_path, &path, &query);
            c->request.path = path;
            if (query) {
                csilk_parse_query(c, query);
                free(query);
            }
        }
    } else {
        char* h_name = csilk_arena_strndup(c->arena, (const char*)name, namelen);
        char* h_value = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
        csilk_set_request_header(c, h_name, h_value);
    }

    return 0;
}

/** @brief Called by nghttp2 after a complete frame has been received. */
int
on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    csilk_client_t* client = (csilk_client_t*)user_data;

    if (frame->hd.type == NGHTTP2_HEADERS && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
        if (c) {
            _csilk_dispatch_request(c);
        }
    } else if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
        if (c) {
            _csilk_dispatch_request(c);
        }
    }

    return 0;
}

/** @brief Called by nghttp2 for each chunk of DATA frame payload. */
int
on_data_chunk_recv_callback(nghttp2_session* session,
                            uint8_t          flags,
                            int32_t          stream_id,
                            const uint8_t*   data,
                            size_t           len,
                            void*            user_data)
{
    (void)session;
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    csilk_ctx_t*    c = csilk_h2_get_or_create_stream(client, stream_id);
    if (!c) {
        return 0;
    }

    if (c->request.body_len + len > _csilk_server_get_max_body_size(client->server)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    size_t req_size = c->request.body_len + len + 1;
    if (c->request.body && c->request.body_capacity >= req_size) {
        memcpy(c->request.body + c->request.body_len, data, len);
        c->request.body_len += len;
        c->request.body[c->request.body_len] = '\0';
    } else {
        size_t cap = 0;
        char*  new_body = (char*)csilk_body_realloc(
            c->request.body, c->request.body_len, c->request.body_capacity, req_size, &cap);
        if (!new_body) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        memcpy(new_body + c->request.body_len, data, len);
        c->request.body_len += len;
        new_body[c->request.body_len] = '\0';
        c->request.body = new_body;
        c->request.body_capacity = cap;
        c->request.body_ownership = CSILK_OWN_POOL;
    }

    return 0;
}

/** @brief Called by nghttp2 when a stream is closed. */
int
on_stream_close_callback(nghttp2_session* session,
                         int32_t          stream_id,
                         uint32_t         error_code,
                         void*            user_data)
{
    (void)session;
    (void)error_code;
    csilk_client_t* client = (csilk_client_t*)user_data;
    if (client) {
        csilk_h2_remove_stream(client, stream_id);
    }
    return 0;
}

/** @brief Response body streaming callback for HTTP/2. */
ssize_t
body_read_callback(nghttp2_session*     session,
                   int32_t              stream_id,
                   uint8_t*             buf,
                   size_t               length,
                   uint32_t*            data_flags,
                   nghttp2_data_source* source,
                   void*                user_data)
{
    (void)session;
    (void)stream_id;
    (void)user_data;
    csilk_ctx_t* c = (csilk_ctx_t*)source->ptr;

    size_t      body_len = c->response.body_len;
    const char* body = (const char*)c->response.body;

    if (!body) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t offset = 0;
    void*  offset_ptr = csilk_get(c, "_h2_body_offset");
    if (offset_ptr) {
        offset = (size_t)(uintptr_t)offset_ptr;
    }

    size_t remaining = body_len - offset;
    size_t to_copy = remaining < length ? remaining : length;

    memcpy(buf, body + offset, to_copy);
    offset += to_copy;

    csilk_set(c, "_h2_body_offset", (void*)(uintptr_t)offset);

    if (offset >= body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/** @brief nghttp2 send callback for writing serialized frames to the client. */
ssize_t
send_callback(
    nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void)session;
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    csilk_client_write(client, data, length);
    return (ssize_t)length;
}

/**
 * @brief Register csilk's internal nghttp2 callbacks on a callbacks object.
 * @param[in] callbacks nghttp2 callbacks struct to populate (created by caller).
 * @note Installs send, frame-recv, data-chunk-recv, stream-close, header, and
 *       begin-headers callbacks used by the HTTP/2 server session.
 */
void
csilk_h2_register_callbacks(nghttp2_session_callbacks* callbacks)
{
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                              on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
}
