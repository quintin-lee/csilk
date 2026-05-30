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
		/* Pseudo-headers */
		if (strncmp((const char*)name, ":method", namelen) == 0) {
			c->request.method =
			    csilk_arena_strndup(c->arena, (const char*)value, valuelen);
		} else if (strncmp((const char*)name, ":path", namelen) == 0) {
			/* Split path and query */
			char* full_path =
			    csilk_arena_strndup(c->arena, (const char*)value, valuelen);
			char* path;
			char* query;
			csilk_split_url(full_path, &path, &query);
			c->request.path = path;
			if (query) {
				csilk_parse_query(c, query);
				free(query); // query is malloc'd by split_url
			}
		}
	} else {
		/* Regular headers */
		char* h_name = csilk_arena_strndup(c->arena, (const char*)name, namelen);
		char* h_value = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
		csilk_set_request_header(c, h_name, h_value);
	}

	return 0;
}

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
	csilk_client_t* client = (csilk_client_t*)user_data;
	csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, stream_id);
	if (!c) {
		return 0;
	}

	/* Accumulate body */
	if (c->request.body_len + len > client->server->config.max_body_size) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	char* new_body = realloc(c->request.body, c->request.body_len + len + 1);
	if (new_body) {
		memcpy(new_body + c->request.body_len, data, len);
		c->request.body_len += len;
		new_body[c->request.body_len] = '\0';
		c->request.body = new_body;
	} else {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	return 0;
}

static ssize_t
body_read_callback(nghttp2_session* session,
		   int32_t stream_id,
		   uint8_t* buf,
		   size_t length,
		   uint32_t* data_flags,
		   nghttp2_data_source* source,
		   void* user_data)
{
	(void)session;
	(void)stream_id;
	(void)user_data;
	csilk_ctx_t* c = (csilk_ctx_t*)source->ptr;

	size_t body_len = c->response.body_len;
	const char* body = (const char*)c->response.body;

	if (!body) {
		*data_flags |= NGHTTP2_DATA_FLAG_EOF;
		return 0;
	}

	/* For simplicity in Phase 2, we assume the whole body fits or we track offset.
       Let's add a temporary storage item for offset. */
	size_t offset = 0;
	void* offset_ptr = csilk_get(c, "_h2_body_offset");
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

void
csilk_h2_send_response(csilk_ctx_t* c)
{
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client || !client->h2_session) {
		return;
	}

	/* Count headers */
	int header_count = 1; /* :status */
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
			header_count++;
		}
	}

	nghttp2_nv* nva = malloc(sizeof(nghttp2_nv) * (size_t)header_count);
	if (!nva) {
		return;
	}

	char status_str[16];
	snprintf(
	    status_str, sizeof(status_str), "%d", c->response.status ? c->response.status : 200);

	nva[0].name = (uint8_t*)":status";
	nva[0].namelen = 7;
	nva[0].value = (uint8_t*)status_str;
	nva[0].valuelen = strlen(status_str);
	nva[0].flags = NGHTTP2_NV_FLAG_NONE;

	int idx = 1;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
			nva[idx].name = (uint8_t*)h->key;
			nva[idx].namelen = h->key_len;
			nva[idx].value = (uint8_t*)h->value;
			nva[idx].valuelen = h->value_len;
			nva[idx].flags = NGHTTP2_NV_FLAG_NONE;
			idx++;
		}
	}

	nghttp2_data_provider prd;
	nghttp2_data_provider* p_prd = NULL;

	if (c->response.body_len > 0) {
		prd.source.ptr = c;
		prd.read_callback = body_read_callback;
		p_prd = &prd;
	}

	nghttp2_submit_response(client->h2_session, c->stream_id, nva, (size_t)header_count, p_prd);
	free(nva);

	nghttp2_session_send(client->h2_session);

	_csilk_trigger_hooks(client->server, c, CSILK_HOOK_REQUEST_END);
}

static int
on_stream_close_callback(nghttp2_session* session,
			 int32_t stream_id,
			 uint32_t error_code,
			 void* user_data)
{
	(void)session;
	(void)error_code;
	csilk_client_t* client = (csilk_client_t*)user_data;

	/* Find and remove context from list */
	csilk_ctx_t** curr = &client->h2_streams;
	while (*curr) {
		if ((*curr)->stream_id == stream_id) {
			csilk_ctx_t* found = *curr;
			*curr = found->next_stream;

			csilk_ctx_cleanup(found);
			if (found->arena) {
				csilk_arena_free(found->arena);
			}
			free(found);
			return 0;
		}
		curr = &((*curr)->next_stream);
	}

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
	ctx->arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);

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
