/**
 * @file http1.c
 * @brief HTTP/1.1 request parsing, response serialization, and sendfile.
 *
 * Implements llHTTP parser callbacks, _csilk_dispatch_request routing,
 * _csilk_send_response serialization, chunked encoding, keep-alive logic,
 * and zero-copy sendfile support.
 * @copyright MIT License
 */

#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "core/srv_internal.h"
#include "core/ctx_internal.h"
#include "h2.h"
#include "srv_impl.h"

/* --- Sendfile completion --- */

static void
on_sendfile_complete(uv_fs_t* req)
{
	csilk_ctx_t* c = (csilk_ctx_t*)req->data;
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	uv_fs_req_cleanup(req);
	free(req);

	if (!client) {
		return;
	}

	int keep_alive = llhttp_should_keep_alive(&client->parser);

	if (client->server->config.write_timeout_ms > 0) {
		uv_timer_stop(&client->write_timer);
	}

	if (keep_alive) {
		uv_timer_start(
		    &client->timer, on_idle_timeout, client->server->config.idle_timeout_ms, 0);
		uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
	} else {
		if (!uv_is_closing((uv_handle_t*)&client->handle)) {
			uv_close((uv_handle_t*)&client->handle, on_close);
		}
	}

	_csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);
	csilk_ctx_cleanup(&client->ctx);
}

/* --- Write completion --- */

/** @brief libuv write completion callback — handles post-write pipeline.
 *
 * After a response body (or TLS-encrypted data) has been written to the
 * socket, this callback orchestrates the next action:
 *
 *   1. If the response includes a file descriptor (file_fd >= 0), the
 *      sendfile pipeline is triggered: uv_fs_sendfile() is called to
 *      stream file data directly from the kernel page cache to the socket.
 *      This path is only used for non-TLS connections.
 *
 *   2. If no file descriptor is pending, the write request is freed and
 *      the connection's keep-alive/close decision is handled by the
 *      caller (_csilk_send_response, which already set up timers).
 *
 *   3. On write error, logs the failure and does NOT retry (the caller
 *      is expected to close the connection via the read callback or timer).
 *
 * The write request's data buffer (buf_copy) is freed here because it
 * was allocated by _csilk_send_data / flush_tls_write.
 *
 * @param req    The completed uv_write_t request.
 * @param status 0 on success, negative on error. */
void
on_write(uv_write_t* req, int status)
{
	if (status < 0) {
		CSILK_LOG_E("Write error: %s", uv_strerror(status));
	}
	csilk_client_t* client = nullptr;
	if (req->handle) {
		client = (csilk_client_t*)req->handle->data;
		if (client) {
			uv_timer_stop(&client->write_timer);
		}
	}

	if (req->data) {
		free(req->data);
	}

	if (client && client->ctx.file_fd >= 0) {
		uv_os_fd_t sock_fd;
		if (uv_fileno((const uv_handle_t*)&client->handle, &sock_fd) == 0) {
			uv_fs_t* fs_req = malloc(sizeof(uv_fs_t));
			if (fs_req) {
				fs_req->data = &client->ctx;
				int fd = client->ctx.file_fd;
				size_t offset = client->ctx.file_offset;
				size_t size = client->ctx.file_size;
				client->ctx.file_fd = -1;

				int r = uv_fs_sendfile(uv_default_loop(),
						       fs_req,
						       sock_fd,
						       fd,
						       offset,
						       size,
						       on_sendfile_complete);
				if (r < 0) {
					free(fs_req);
				} else {
					free(req);
					return;
				}
			}
		}
	}

	free(req);
}

/* --- llHTTP parser callbacks --- */

/** @brief llhttp callback: HTTP message start.
 *
 * Called at the beginning of each HTTP request message (including
 * keep-alive connections). Resets header tracking counters and restarts
 * the request timeout timer.
 *
 * @param p The llhttp parser instance (data points to csilk_client_t).
 * @return 0 (HPE_OK) to continue parsing. */
int
on_message_begin(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size = 0;
	client->header_count = 0;

	if (client->server->config.request_timeout_ms > 0) {
		uv_timer_stop(&client->request_timer);
		uv_timer_start(&client->request_timer,
			       on_read_timeout,
			       client->server->config.request_timeout_ms,
			       0);
	}

	csilk_log_set_request_id(nullptr);

	/* Free the previous request's header buffers; new ones will be
	 * allocated by buf_grow in on_header_field/on_header_value. */
	if (client->current_header_field) {
		free(client->current_header_field);
		client->current_header_field = nullptr;
		client->header_field_capacity = 0;
	}
	if (client->current_header_value) {
		free(client->current_header_value);
		client->current_header_value = nullptr;
		client->header_value_capacity = 0;
	}
	if (client->current_url) {
		free(client->current_url);
		client->current_url = nullptr;
		client->current_url_capacity = 0;
	}

	return 0;
}

/* Forward declaration for buffer reuse helper used by URL and header callbacks */
static char* buf_grow(char* buf, size_t* cap, size_t needed);

/** @brief llhttp callback: URL data received.
 *
 * Stores the raw URL string. Checks against max_url_size and returns
 * HPE_USER if exceeded (aborts parsing).
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to the URL data.
 * @param length Length of the URL data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if URL exceeds max_url_size. */
int
on_url(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	size_t max_url = client->server->config.max_url_size;
	if (max_url > 0 && length > max_url) {
		CSILK_LOG_W("URL length (%zu) exceeds max_url_size limit (%zu)", length, max_url);
		free(client->current_url);
		client->current_url = nullptr;
		return HPE_USER;
	}
	if (client->current_url) {
		/* Reset length, keep capacity for reuse via buf_grow */
		client->current_url[0] = '\0';
	}
	if (!buf_grow(client->current_url, &client->current_url_capacity, length + 1)) {
		CSILK_LOG_E("Failed to allocate/grow memory for URL string");
		client->current_url = nullptr;
		client->current_url_capacity = 0;
		return HPE_USER;
	}
	memcpy(client->current_url, at, length);
	client->current_url[length] = '\0';
	return 0;
}

/** @brief llhttp callback: header field name received.
 *
 * Accumulates header field names. When a previous field+value pair is
 * complete, stores it in the request context. Enforces max_header_size and
 * max_headers_count limits (returns HPE_USER on violation).
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to header field data.
 * @param length Length of the header field data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if size/count limits are exceeded. */
int
on_header_field(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size += length;
	if (client->total_header_size > client->server->config.max_header_size) {
		CSILK_LOG_W("Total header size limit exceeded on header field");
		return HPE_USER;
	}
	client->header_count++;
	if (client->server->config.max_headers_count > 0 &&
	    client->header_count > client->server->config.max_headers_count) {
		CSILK_LOG_W("Total header count limit exceeded (%d)", client->header_count);
		return HPE_USER;
	}

	if (client->current_header_field && client->current_header_value) {
		csilk_set_request_header(
		    &client->ctx, client->current_header_field, client->current_header_value);
		/* Reuse buffers via buf_grow — reset length, keep capacity */
		client->current_header_field[0] = '\0';
		client->current_header_value[0] = '\0';
	} else if (client->current_header_field) {
		client->current_header_field[0] = '\0';
	}

	/* Use buf_grow to reuse the buffer across headers (avoids malloc/free per header) */
	if (!buf_grow(client->current_header_field, &client->header_field_capacity, length + 1)) {
		CSILK_LOG_E("Failed to allocate/grow memory for header field");
		client->current_header_field = nullptr;
		client->header_field_capacity = 0;
		return HPE_USER;
	}
	memcpy(client->current_header_field, at, length);
	client->current_header_field[length] = '\0';
	return 0;
}

/** @brief Grow a heap-allocated buffer to at least @p needed bytes.
 *
 * Uses realloc with capacity doubling for amortized O(1) growth. If @p buf
 * is nullptr and *@p cap is 0, this acts as a malloc. On realloc failure the
 * original buffer is NOT freed (caller must free it).
 *
 * @param buf    Existing allocation (may be nullptr).
 * @param cap    [in,out] Current capacity — updated on success.
 * @param needed Minimum required size in bytes.
 * @return Pointer to the resized buffer, or nullptr on allocation failure. */
static char*
buf_grow(char* buf, size_t* cap, size_t needed)
{
	if (needed <= *cap) {
		return buf;
	}
	size_t new_cap = *cap ? *cap : 32;
	while (new_cap < needed) {
		if (new_cap > SIZE_MAX / 2) {
			return nullptr;
		}
		new_cap *= 2;
	}
	char* new_buf = realloc(buf, new_cap);
	if (!new_buf) {
		return nullptr;
	}
	*cap = new_cap;
	return new_buf;
}

/** @brief llhttp callback: header value data received.
 *
 * Appends to the current header value buffer. Enforces max_header_size
 * limit (returns HPE_USER if exceeded). On allocation failure, frees the
 * partial value and returns HPE_USER.
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to header value data.
 * @param length Length of header value data.
 * @return 0 (HPE_OK) on success, HPE_USER if size limit or allocation fails. */
int
on_header_value(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size += length;
	if (client->total_header_size > client->server->config.max_header_size) {
		CSILK_LOG_W("Total header size limit exceeded on header value");
		free(client->current_header_field);
		client->current_header_field = nullptr;
		free(client->current_header_value);
		client->current_header_value = nullptr;
		return HPE_USER;
	}

	size_t prev_len = client->current_header_value ? strlen(client->current_header_value) : 0;
	size_t needed = prev_len + length + 1;
	char* new_val =
	    buf_grow(client->current_header_value, &client->header_value_capacity, needed);
	if (!new_val) {
		free(client->current_header_value);
		client->current_header_value = nullptr;
		client->header_value_capacity = 0;
		client->total_header_size = 0;
		free(client->current_header_field);
		client->current_header_field = nullptr;
		CSILK_LOG_E("Failed to allocate/grow memory for header value");
		return HPE_USER;
	}
	client->current_header_value = new_val;
	memcpy(client->current_header_value + prev_len, at, length);
	client->current_header_value[prev_len + length] = '\0';
	return 0;
}

/** @brief llhttp callback: all HTTP headers have been received.
 *
 * Flushes any remaining header field+value pair into the request context.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) to continue parsing. */
int
on_headers_complete(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	if (client->current_header_field && client->current_header_value) {
		csilk_set_request_header(
		    &client->ctx, client->current_header_field, client->current_header_value);
		free(client->current_header_field);
		client->current_header_field = nullptr;
		client->header_field_capacity = 0;
		free(client->current_header_value);
		client->current_header_value = nullptr;
		client->header_value_capacity = 0;
	}
	return 0;
}

/** @brief llhttp callback: body data received.
 *
 * Appends body data to the request body buffer (realloc as needed). Enforces
 * max_body_size limit (returns HPE_USER if exceeded). On realloc failure,
 * the existing body is freed and HPE_USER is returned.
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to body data.
 * @param length Length of body data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if the body exceeds max_body_size. */
int
on_body(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	if (client->ctx.request.body_len + length > client->server->config.max_body_size) {
		return HPE_USER;
	}

	/* Overflow check for size calculation: body_len + length + 1 */
	if (length > SIZE_MAX - client->ctx.request.body_len - 1) {
		return HPE_USER;
	}

	char* new_body =
	    realloc(client->ctx.request.body, client->ctx.request.body_len + length + 1);
	if (new_body) {
		memcpy(new_body + client->ctx.request.body_len, at, length);
		client->ctx.request.body_len += length;
		new_body[client->ctx.request.body_len] = '\0';
		client->ctx.request.body = new_body;
	} else {
		free(client->ctx.request.body);
		client->ctx.request.body = nullptr;
		client->ctx.request.body_len = 0;
		return HPE_USER;
	}
	return 0;
}

/** @brief Map an HTTP status code to its standard reason phrase.
 *
 * Supports common codes: 101, 200, 201, 204, 400, 401, 403, 404, 500.
 * Unrecognized codes default to "OK".
 *
 * @param status HTTP status code.
 * @return A static string literal with the reason phrase. */
static const char*
get_status_text(int status)
{
	switch (status) {
	case CSILK_STATUS_SWITCHING_PROTOCOLS:
		return "Switching Protocols";
	case CSILK_STATUS_OK:
		return "OK";
	case CSILK_STATUS_CREATED:
		return "Created";
	case CSILK_STATUS_NO_CONTENT:
		return "No Content";
	case CSILK_STATUS_BAD_REQUEST:
		return "Bad Request";
	case CSILK_STATUS_UNAUTHORIZED:
		return "Unauthorized";
	case CSILK_STATUS_FORBIDDEN:
		return "Forbidden";
	case CSILK_STATUS_NOT_FOUND:
		return "Not Found";
	case CSILK_STATUS_INTERNAL_SERVER_ERROR:
		return "Internal Server Error";
	default:
		return "OK";
	}
}

/* --- Client write --- */

/** @brief Send raw data to the client (TLS-aware).
 *
 * If TLS is active, writes through the SSL session and flushes the write BIO.
 * Otherwise, allocates a write request, copies the data, and queues the write
 * via libuv. The data buffer is freed by the write completion callback.
 *
 * @param client The client connection.
 * @param data   Data buffer to send.
 * @param len    Length of data in bytes. */
void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t len)
{
	if (!client) {
		return;
	}

	if (client->ssl) {
		SSL_write(client->ssl, data, (int)len);
		flush_tls_write(client);
		return;
	}

	uv_write_t* req = malloc(sizeof(uv_write_t));
	if (!req) {
		return;
	}

	char* buf_copy = malloc(len);
	if (!buf_copy) {
		free(req);
		return;
	}
	memcpy(buf_copy, data, len);

	uv_buf_t buf = uv_buf_init(buf_copy, (unsigned int)len);
	req->data = buf_copy;
	uv_write(req, (uv_stream_t*)&client->handle, &buf, 1, on_write);
}

/* --- Send data --- */

/** @brief Write raw data to the client connection.
 *
 * Extracts the internal client from the context and delegates the write
 * to the client's buffered write path. Used internally by the response
 * sender to flush serialised HTTP data.
 *
 * @param c    The request context.
 * @param data Pointer to the data buffer.
 * @param len  Number of bytes to write. */
CSILK_INTERNAL void
_csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	csilk_client_write(client, data, len);
}

/** @brief Send data with ownership transfer — caller's buffer is freed by
 *  the write callback (or immediately for TLS).
 *
 *  Unlike _csilk_send_data() / csilk_client_write(), this does NOT make
 *  an internal copy. Instead the caller's heap buffer is passed directly
 *  to uv_write() and freed by on_write().  For TLS connections the buffer
 *  is freed immediately after SSL_write(). */
CSILK_INTERNAL void
_csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len)
{
	if (!data) {
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client) {
		free(data);
		return;
	}

	if (client->ssl) {
		SSL_write(client->ssl, (const uint8_t*)data, (int)len);
		flush_tls_write(client);
		free(data);
		return;
	}

	uv_write_t* req = malloc(sizeof(uv_write_t));
	if (!req) {
		free(data);
		return;
	}

	req->data = data;
	uv_buf_t buf = uv_buf_init(data, (unsigned int)len);
	uv_write(req, (uv_stream_t*)&client->handle, &buf, 1, on_write);
}

/* --- Request dispatch --- */

/** @brief Dispatch an incoming request through the middleware and routing
 * pipeline.
 *
 * Triggers the CSILK_HOOK_REQUEST_BEGIN lifecycle hook, then attempts to
 * match the request path against the server's radix-tree router.  On a
 * match the route-level handlers are prepended with any registered global
 * middlewares and execution begins via csilk_next().  On a miss the
 * not-found handler (or a default 404 string response) is invoked.
 *
 * After dispatch the function handles the async / sync split: for async
 * handlers the HTTP/1 connection read loop is stopped (the handler is
 * responsible for sending the response later); for sync handlers the
 * response is sent immediately via _csilk_send_response().
 *
 * @param c The request context populated by the HTTP parser. */
CSILK_INTERNAL void
_csilk_dispatch_request(csilk_ctx_t* c)
{
	if (!c || !c->server) {
		return;
	}

	csilk_server_t* server = (csilk_server_t*)c->server;

	CSILK_LOG_I("Request: %s %s", c->request.method, c->request.path);

	_csilk_trigger_hooks(server, c, CSILK_HOOK_REQUEST_BEGIN);

	if (csilk_router_match_ctx(server->router, c)) {
		CSILK_LOG_D("Route matched, calling next handler");

		if (server->middleware_count > 0) {
			int route_handler_count = 0;
			while (c->handlers[route_handler_count] != nullptr) {
				route_handler_count++;
			}

			int total_count = server->middleware_count + route_handler_count;
			csilk_handler_t* arena_handlers = csilk_arena_alloc(
			    c->arena, (total_count + 1) * sizeof(csilk_handler_t));
			if (arena_handlers) {
				for (int i = 0; i < server->middleware_count; i++) {
					arena_handlers[i] = server->middlewares[i];
				}
				for (int i = 0; i < route_handler_count; i++) {
					arena_handlers[server->middleware_count + i] =
					    c->handlers[i];
				}
				arena_handlers[total_count] = nullptr;
				c->handlers = arena_handlers;
			}
		}

		csilk_next(c);
	} else {
		CSILK_LOG_W("Route not found: %s", c->request.path);
		if (server->not_found_handler) {
			server->not_found_handler(c);
		} else {
			csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		}
	}

	if (c->is_async) {
		csilk_client_t* client = (csilk_client_t*)c->_internal_client;
		if (client && client->protocol == CSILK_PROTO_HTTP1) {
			uv_read_stop((uv_stream_t*)&client->handle);
		}
	}

	if (!c->is_async) {
		_csilk_send_response(c);
	}
}

/* --- Response serialization --- */

/** @brief Send the assembled HTTP response to the client.
 *
 * This is the central response-serialization function. It constructs the
 * HTTP response bytes in memory and sends them via _csilk_send_data().
 * The response format is:
 *
 *   HTTP/1.1 <status> <reason>\r\n
 *   [Transfer-Encoding: chunked\r\n]
 *   Content-Length: <len>\r\n
 *   Connection: keep-alive|close\r\n
 *   <custom headers...>\r\n
 *   \r\n
 *   <body>
 *
 * Response mode selection:
 *   - Normal (Sync):       Content-Length is set to body_len; body is
 *                          appended inline after the header block.
 *   - Chunked (Async):     No Content-Length; Transfer-Encoding: chunked
 *                          is set. Used when the handler calls csilk_next()
 *                          with is_async = true, meaning it may write
 *                          response chunks over time.
 *   - File (sendfile):     Content-Length is set to file_size; the header
 *                          is sent via _csilk_send_data, then on_write
 *                          triggers uv_fs_sendfile for zero-copy file
 *                          delivery. Only available on non-TLS connections.
 *   - WebSocket (101):     Minimal header; the caller manages frames via
 *                          csilk_ws_send(). See is_websocket branch.
 *
 * After the response is sent:
 *   - For sendfile: return early, defer cleanup to on_sendfile_complete.
 *   - For keep-alive: restart the idle timer, begin reading next request.
 *   - For close: initiate uv_close.
 *   - Fire CSILK_HOOK_REQUEST_END, clean up context.
 *
 * @param c Request context (must have _internal_client set). */
CSILK_INTERNAL void
_csilk_send_response(csilk_ctx_t* c)
{
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client) {
		return;
	}

	if (client->protocol == CSILK_PROTO_HTTP2) {
		csilk_h2_send_response(c);
		return;
	}

	uv_timer_stop(&client->request_timer);

	int status = client->ctx.response.status ? client->ctx.response.status : 200;
	const char* status_text = get_status_text(status);

	int is_file = (c->file_fd >= 0 && !client->ssl);
	int use_chunked = (client->ctx.response.body_len == 0 && client->ctx.is_async && !is_file);
	const char* transfer_encoding = use_chunked ? "Transfer-Encoding: chunked\r\n" : "";

	size_t custom_headers_len = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h; h = h->next) {
			custom_headers_len += h->key_len + 2 + h->value_len + 2;
		}
	}

	size_t body_len = is_file ? c->file_size : client->ctx.response.body_len;
	const char* body = client->ctx.response.body ? client->ctx.response.body : "";

	int keep_alive = llhttp_should_keep_alive(&client->parser);
	const char* connection_val = keep_alive ? "keep-alive" : "close";

	int header_len;
	if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
		header_len = snprintf(nullptr, 0, "HTTP/1.1 101 Switching Protocols\r\n");
	} else if (use_chunked) {
		header_len = snprintf(nullptr,
				      0,
				      "HTTP/1.1 %d %s\r\n"
				      "%s"
				      "Connection: %s\r\n",
				      status,
				      status_text,
				      transfer_encoding,
				      connection_val);
	} else {
		header_len = snprintf(nullptr,
				      0,
				      "HTTP/1.1 %d %s\r\n"
				      "Content-Length: %zu\r\n"
				      "Connection: %s\r\n",
				      status,
				      status_text,
				      body_len,
				      connection_val);
	}

	if (header_len < 0) {
		return;
	}

	size_t response_len =
	    (size_t)header_len + custom_headers_len + 2 + (use_chunked || is_file ? 0 : body_len);

	char* write_base = malloc(response_len + 1);
	if (write_base) {
		int snp;
		size_t pos;
		if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
			snp = snprintf(
			    write_base, response_len + 1, "HTTP/1.1 101 Switching Protocols\r\n");
		} else if (use_chunked) {
			snp = snprintf(write_base,
				       response_len + 1,
				       "HTTP/1.1 %d %s\r\n"
				       "%s"
				       "Connection: %s\r\n",
				       status,
				       status_text,
				       transfer_encoding,
				       connection_val);
		} else {
			snp = snprintf(write_base,
				       response_len + 1,
				       "HTTP/1.1 %d %s\r\n"
				       "Content-Length: %zu\r\n"
				       "Connection: %s\r\n",
				       status,
				       status_text,
				       body_len,
				       connection_val);
		}
		if (snp < 0) {
			free(write_base);
			return;
		}
		pos = (size_t)snp;

		for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
			for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h;
			     h = h->next) {
				memcpy(write_base + pos, h->key, h->key_len);
				pos += h->key_len;
				write_base[pos++] = ':';
				write_base[pos++] = ' ';
				memcpy(write_base + pos, h->value, h->value_len);
				pos += h->value_len;
				write_base[pos++] = '\r';
				write_base[pos++] = '\n';
			}
		}

		if (!use_chunked && !is_file) {
			snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n%s", body);
		} else {
			snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n");
		}

		_csilk_send_data(c,
				 (const uint8_t*)write_base,
				 (use_chunked || is_file ? (size_t)pos + 2 : response_len));
		free(write_base);
	}

	if (is_file) {
		return;
	}

	uv_timer_stop(&client->read_timer);

	if (client->server->config.write_timeout_ms > 0) {
		uv_timer_start(&client->write_timer,
			       on_write_timeout,
			       client->server->config.write_timeout_ms,
			       0);
	}

	if (client->ctx.is_websocket) {
	} else {
		if (keep_alive) {
			uv_timer_start(&client->timer,
				       on_idle_timeout,
				       client->server->config.idle_timeout_ms,
				       0);
			uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
		} else {
			if (!uv_is_closing((uv_handle_t*)&client->handle)) {
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
		}
	}

	int is_ws = client->ctx.is_websocket;
	void* ws_msg_cb = client->ctx.on_ws_message;
	void* ws_send_cb = client->ctx.on_ws_send;

	_csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);

	csilk_ctx_cleanup(&client->ctx);

	if (is_ws) {
		client->ctx.is_websocket = is_ws;
		client->ctx.on_ws_message = ws_msg_cb;
		client->ctx.on_ws_send = ws_send_cb;
	}
}

/* --- Request finalization --- */

/** @brief Finalize the parsed request data before routing.
 *
 * Stores any remaining header field+value pair, splits the URL into path
 * and query string, URL-decodes the path, parses query parameters into the
 * context's query_params map, and sets the HTTP method on the context.
 *
 * @param client The client connection.
 * @param p      The llhttp parser instance. */
static void
finalize_request(csilk_client_t* client, llhttp_t* p)
{
	if (client->current_header_field && client->current_header_value) {
		csilk_set_request_header(
		    &client->ctx, client->current_header_field, client->current_header_value);
		free(client->current_header_field);
		client->current_header_field = nullptr;
		client->header_field_capacity = 0;
		free(client->current_header_value);
		client->current_header_value = nullptr;
		client->header_value_capacity = 0;
	}

	if (client->current_url) {
		char* path = nullptr;
		char* query = nullptr;
		csilk_split_url(client->current_url, &path, &query);
		if (client->ctx.request.path) {
			free((void*)client->ctx.request.path);
		}
		client->ctx.request.path = path;
		if (query) {
			csilk_parse_query(&client->ctx, query);
			free(query);
		}
		free(client->current_url);
		client->current_url = nullptr;
	}

	client->ctx.request.method = (char*)llhttp_method_name(llhttp_get_method(p));
}

/* --- Message complete --- */

/** @brief llhttp callback: the full HTTP request message has been parsed.
 *
 * This is the main request dispatch point. It executes the following
 * pipeline for every incoming HTTP request:
 *
 *   1. finalize_request(): store remaining headers, split URL into path
 *      and query, URL-decode the path, parse query parameters.
 *
 *   2. _csilk_dispatch_request(): trigger hooks, match route, prepend
 *      global middlewares, execute handler chain, send response.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) on success, non-zero to abort parsing. */
int
on_message_complete(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;

	finalize_request(client, p);
	_csilk_dispatch_request(&client->ctx);

	return 0;
}
