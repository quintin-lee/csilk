/**
 * @file h2_callbacks.c
 * @brief nghttp2 callback implementations for HTTP/2 session handling.
 *
 * Split from h2.c. Contains all static nghttp2 callback functions and
 * a registration helper that wires them into an nghttp2_session_callbacks.
 *
 * @copyright MIT License
 */

#include "h2.h"
#include "csilk/csilk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Called by nghttp2 when a HEADERS frame is received and parsing begins. */
static int
on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    (void)session;
    (void)frame;
    (void)user_data;
    return 0;
}

/** @brief Called by nghttp2 for each header name/value pair in a HEADERS frame. */
static int
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
static int
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
static int
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

    if (c->request.body_len + len > client->server->config.max_body_size) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    char* new_body = realloc(c->request.body, c->request.body_len + len + 1);
    if (new_body) {
        memcpy(new_body + c->request.body_len, data, len);
        c->request.body_len += len;
        new_body[c->request.body_len] = '\0';
        c->request.body = new_body;
        c->request.body_is_managed = 1;
    } else {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}

/** @brief Called by nghttp2 when a stream is closed. */
static int
on_stream_close_callback(nghttp2_session* session,
                         int32_t          stream_id,
                         uint32_t         error_code,
                         void*            user_data)
{
    (void)session;
    (void)error_code;
    csilk_client_t* client = (csilk_client_t*)user_data;

    csilk_ctx_t** curr = &client->h2_streams;
    while (*curr) {
        if ((*curr)->stream_id == stream_id) {
            csilk_ctx_t* found = *curr;
            *curr = found->next_stream;

            csilk_ctx_cleanup(found);
            if (found->arena) {
                csilk_arena_free(found->arena);
                found->arena = NULL;
            }

            free(found);
            return 0;
        }
        curr = &((*curr)->next_stream);
    }

    return 0;
}

/** @brief nghttp2 send callback for writing serialized frames to the client. */
static ssize_t
send_callback(
    nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void)session;
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    csilk_client_write(client, data, length);
    return (ssize_t)length;
}

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
