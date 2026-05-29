/**
 * @file h2.c
 * @brief HTTP/2 integration for csilk.
 *
 * @copyright MIT License
 */

#include "h2.h"
#include "csilk/csilk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
	(void)session;
	(void)frame;
	(void)user_data;
	return 0;
}

static int
on_header_callback(nghttp2_session* session,
		   const nghttp2_frame* frame,
		   const uint8_t* name,
		   size_t namelen,
		   const uint8_t* value,
		   size_t valuelen,
		   uint8_t flags,
		   void* user_data)
{
	(void)session;
	(void)frame;
	(void)name;
	(void)namelen;
	(void)value;
	(void)valuelen;
	(void)flags;
	(void)user_data;
	return 0;
}

static int
on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
	(void)session;
	(void)frame;
	(void)user_data;
	return 0;
}

static int
on_data_chunk_recv_callback(nghttp2_session* session,
			    uint8_t flags,
			    int32_t stream_id,
			    const uint8_t* data,
			    size_t len,
			    void* user_data)
{
	(void)session;
	(void)flags;
	(void)stream_id;
	(void)data;
	(void)len;
	(void)user_data;
	return 0;
}

static int
on_stream_close_callback(nghttp2_session* session,
			 int32_t stream_id,
			 uint32_t error_code,
			 void* user_data)
{
	(void)session;
	(void)stream_id;
	(void)error_code;
	(void)user_data;
	return 0;
}

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
	ctx->arena = csilk_arena_new(
	    client->server->config.arena_size ? client->server->config.arena_size : 4096);

	/* Prepend to list */
	ctx->next_stream = client->h2_streams;
	client->h2_streams = ctx;

	return ctx;
}

void
csilk_h2_free_streams(csilk_client_t* client)
{
	csilk_ctx_t* curr = client->h2_streams;
	while (curr) {
		csilk_ctx_t* next = curr->next_stream;
		csilk_ctx_cleanup(curr);
		if (curr->arena) {
			csilk_arena_free(curr->arena);
		}
		free(curr);
		curr = next;
	}
	client->h2_streams = NULL;
}

int
csilk_h2_init_session(csilk_client_t* client)

{
	nghttp2_session_callbacks* callbacks;
	if (nghttp2_session_callbacks_new(&callbacks) != 0) {
		return -1;
	}

	nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
	nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
								  on_data_chunk_recv_callback);
	nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
	nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
	nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,
								on_begin_headers_callback);

	if (nghttp2_session_server_new(&client->h2_session, callbacks, client) != 0) {
		nghttp2_session_callbacks_del(callbacks);
		return -1;
	}

	nghttp2_session_callbacks_del(callbacks);

	nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};

	if (nghttp2_submit_settings(client->h2_session, NGHTTP2_FLAG_NONE, iv, 1) != 0) {
		return -1;
	}

	nghttp2_session_send(client->h2_session);

	return 0;
}

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
