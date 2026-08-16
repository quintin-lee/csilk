/**
 * @file h2_session.c
 * @brief HTTP/2 session and stream management.
 */

#include "h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Stream lookup/creation --- */

csilk_ctx_t*
csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id)
{
    csilk_ctx_t* curr = client->h2_streams;
    while (curr) {
        if (curr->stream_id == stream_id) {
            return curr;
        }
        curr = curr->next_stream;
    }

    /* Create new context for stream */
    csilk_ctx_t* ctx = malloc(sizeof(csilk_ctx_t));
    if (!ctx) {
        return NULL;
    }

    _csilk_ctx_init(ctx, client->server, client);
    ctx->stream_id = stream_id;
    ctx->arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    if (client->server->config.enable_arena_alignment) {
        csilk_arena_set_alignment(ctx->arena, 1);
    }

    /* Prepend to list */
    ctx->next_stream = client->h2_streams;
    client->h2_streams = ctx;

    return ctx;
}

/**
 * @brief Free all HTTP/2 streams associated with a client connection.
 * @param[in] client Client whose h2_streams list is torn down.
 */
void
csilk_h2_free_streams(csilk_client_t* client)
{
    csilk_ctx_t* curr = client->h2_streams;
    while (curr) {
        csilk_ctx_t* next = curr->next_stream;
        csilk_ctx_cleanup(curr);
        if (curr->arena) {
            csilk_arena_free(curr->arena);
            curr->arena = NULL;
        }
        free(curr);
        curr = next;
    }
    client->h2_streams = NULL;
}

/* --- Session initialization --- */

int
csilk_h2_init_session(csilk_client_t* client)
{
    nghttp2_session_callbacks* callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return -1;
    }

    extern int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_header_callback(nghttp2_session*,
                                  const nghttp2_frame*,
                                  const uint8_t*,
                                  size_t,
                                  const uint8_t*,
                                  size_t,
                                  uint8_t,
                                  void*);
    extern int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_data_chunk_recv_callback(
        nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void*);
    extern int     on_stream_close_callback(nghttp2_session*, int32_t, uint32_t, void*);
    extern ssize_t send_callback(nghttp2_session*, const uint8_t*, size_t, int, void*);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                              on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);

    if (nghttp2_session_server_new(&client->h2_session, callbacks, client) != 0) {
        nghttp2_session_callbacks_del(callbacks);
        return -1;
    }

    nghttp2_session_callbacks_del(callbacks);

    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };

    if (nghttp2_submit_settings(client->h2_session, NGHTTP2_FLAG_NONE, iv, 1) != 0) {
        return -1;
    }

    nghttp2_session_send(client->h2_session);

    return 0;
}

/* --- Data processing --- */

int
csilk_h2_process_data(csilk_client_t* client, const uint8_t* data, size_t len)
{
    ssize_t rv = nghttp2_session_mem_recv(client->h2_session, data, len);
    if (rv < 0) {
        return -1;
    }

    if (nghttp2_session_send(client->h2_session) != 0) {
        return -1;
    }

    return 0;
}
