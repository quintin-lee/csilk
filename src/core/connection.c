/**
 * @file connection.c
 * @brief Connection pool, accept, I/O, timers, and lifecycle callbacks.
 *
 * Implements client connection pooling, accept handling (on_new_connection),
 * TCP read/write callbacks, idle/timeout timers, and client lifecycle
 * (on_close, on_timer_close).  All I/O flows through the on_read callback
 * which dispatches to TLS, WebSocket, or HTTP/1.1 protocol handlers.
 * @copyright MIT License
 */

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/srv_types.h"
#include "csilk/core/ctx_types.h"
#include "h2.h"
#include "srv_impl.h"

/* --- Buffer allocation --- */

/** @brief libuv buffer allocation callback — allocates a receive buffer.
 *
 * Allocates a buffer of the suggested size using malloc. The buffer is freed
 * by libuv after the read callback is invoked.
 *
 * @param handle          The libuv handle that will read into the buffer.
 * @param suggested_size  Recommended buffer size from libuv.
 * @param buf             [out] Pointer to the uv_buf_t to populate. */
void
alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	(void)handle;
	buf->base = (char*)malloc(suggested_size);
	buf->len = suggested_size;
}

/* --- Connection pool --- */

/** @brief Get a client connection object from the server's free pool or
 * allocate a new one.
 *
 * Reuses a previously freed client if available (up to 32 pooled entries),
 * otherwise allocates a new zero-initialized csilk_client_t. The returned
 * client's file_fd is initialized to -1.
 *
 * @param server The server instance.
 * @return A csilk_client_t ready for use, or NULL on allocation failure. */
static csilk_client_t*
pool_get(csilk_server_t* server)
{
	csilk_client_t* client;
	uv_mutex_lock(&server->pool_mutex);
	if (server->client_pool_count > 0) {
		client = server->client_pool[--server->client_pool_count];
	} else {
		client = calloc(1, sizeof(csilk_client_t));
	}
	uv_mutex_unlock(&server->pool_mutex);
	if (client) {
		client->ctx.file_fd = -1;
	}
	return client;
}

/** @brief Return a client connection to the server's free pool for reuse.
 *
 * If the client has an SSL session, it is freed first. The client struct is
 * zeroed. If the pool has fewer than 32 entries, the client is saved for
 * reuse; otherwise it is freed.
 *
 * @param server The server instance.
 * @param client The client to return (must not be used after this call). */
static void
pool_put(csilk_server_t* server, csilk_client_t* client)
{
	if (client->ssl) {
		SSL_free(client->ssl);
		client->ssl = NULL;
		client->read_bio = NULL;
		client->write_bio = NULL;
	}
	if (client->h2_session) {
		nghttp2_session_del(client->h2_session);
		client->h2_session = NULL;
	}
	csilk_h2_free_streams(client);
	memset(client, 0, sizeof(*client));
	uv_mutex_lock(&server->pool_mutex);
	if (server->client_pool_count < 32) {
		server->client_pool[server->client_pool_count++] = client;
	} else {
		free(client);
	}
	uv_mutex_unlock(&server->pool_mutex);
}

/* --- Active client list --- */

/** @brief Insert a client at the head of the server's active client list.
 *
 * Thread-safe: acquires the clients_mutex before modification.
 *
 * @param server The server instance.
 * @param client The client to add (must not already be in a list). */
static void
client_list_add(csilk_server_t* server, csilk_client_t* client)
{
	uv_mutex_lock(&server->clients_mutex);
	client->next = server->active_clients;
	client->prev = NULL;
	if (server->active_clients) {
		server->active_clients->prev = client;
	}
	server->active_clients = client;
	uv_mutex_unlock(&server->clients_mutex);
}

/** @brief Remove a client from the active list (no locking).
 *
 * Unlinks the client from the doubly-linked list and clears its prev/next
 * pointers. Caller must hold clients_mutex.
 *
 * @param server The server instance.
 * @param client The client to remove. */
static void
client_list_remove_internal(csilk_server_t* server, csilk_client_t* client)
{
	if (client->prev) {
		client->prev->next = client->next;
	} else if (server->active_clients == client) {
		server->active_clients = client->next;
	}
	if (client->next) {
		client->next->prev = client->prev;
	}
	client->next = client->prev = NULL;
}

/** @brief Remove a client from the active list (thread-safe).
 *
 * Acquires clients_mutex, then delegates to client_list_remove_internal().
 *
 * @param server The server instance.
 * @param client The client to remove. */
static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
	uv_mutex_lock(&server->clients_mutex);
	client_list_remove_internal(server, client);
	uv_mutex_unlock(&server->clients_mutex);
}

/* --- Timer close --- */

/** @brief libuv close callback for client timer handles.
 *
 * Decrements the close_pending counter. When all four timers are closed
 * (close_pending reaches 0), the client is fully cleaned up: the arena is
 * freed, temporary fields are freed, and the client is returned to the pool.
 *
 * @param handle The timer handle being closed (data points to csilk_client_t).
 */
static void
on_timer_close(uv_handle_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (!client) {
		return;
	}
	client->close_pending--;
	if (client->close_pending > 0) {
		return;
	}

	if (client->server) {
		atomic_fetch_sub(&client->server->active_connections, 1);
	}
	csilk_ctx_cleanup(&client->ctx);
	if (client->ctx.arena) {
		csilk_arena_free(client->ctx.arena);
	}
	free(client->current_header_field);
	free(client->current_header_value);
	free(client->current_url);
	csilk_server_t* srv = client->server;
	pool_put(srv, client);
}

/* --- Connection close --- */

/** @brief libuv close callback for client TCP handles — performs full cleanup.
 *
 * Triggers the CSILK_HOOK_CONN_CLOSE hook, removes the client from the
 * active connections list, stops all four timers, and initiates their close
 * via on_timer_close. When all timers are closed, the client's request
 * context, arena, and temporary buffers are freed and the client is returned
 * to the pool.
 *
 * @param handle The TCP handle being closed (data points to csilk_client_t).
 */
void
on_close(uv_handle_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (client) {
		_csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
		client_list_remove(client->server, client);
		client->ctx._internal_client = NULL;
		uv_timer_stop(&client->timer);
		uv_timer_stop(&client->read_timer);
		uv_timer_stop(&client->write_timer);
		uv_timer_stop(&client->request_timer);

		client->close_pending = 4;
		uv_handle_t* timers[] = {(uv_handle_t*)&client->timer,
					 (uv_handle_t*)&client->read_timer,
					 (uv_handle_t*)&client->write_timer,
					 (uv_handle_t*)&client->request_timer};
		for (int i = 0; i < 4; i++) {
			if (uv_is_closing(timers[i])) {
				client->close_pending--;
			} else {
				timers[i]->data = client;
				uv_close(timers[i], on_timer_close);
			}
		}
		if (client->close_pending <= 0) {
			csilk_server_t* srv = client->server;
			if (srv) {
				atomic_fetch_sub(&srv->active_connections, 1);
			}
			csilk_ctx_cleanup(&client->ctx);
			if (client->ctx.arena) {
				csilk_arena_free(client->ctx.arena);
			}
			free(client->current_header_field);
			free(client->current_header_value);
			free(client->current_url);
			pool_put(srv, client);
		}
	}
}

/* --- Timer callbacks --- */

void
on_idle_timeout(uv_timer_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (!uv_is_closing((uv_handle_t*)&client->handle)) {
		CSILK_LOG_D("Closing connection: idle timeout");
		uv_close((uv_handle_t*)&client->handle, on_close);
	}
}

/** @brief libuv timer callback: fired when no request data has been received
 * within read_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data). */
void
on_read_timeout(uv_timer_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (!uv_is_closing((uv_handle_t*)&client->handle)) {
		CSILK_LOG_D("Closing connection: read timeout");
		uv_close((uv_handle_t*)&client->handle, on_close);
	}
}

/** @brief libuv timer callback: fired when the response write has not
 * completed within write_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data). */
void
on_write_timeout(uv_timer_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (!uv_is_closing((uv_handle_t*)&client->handle)) {
		uv_close((uv_handle_t*)&client->handle, on_close);
	}
}

/* --- Rejected connection --- */

static void
on_rejected_close(uv_handle_t* handle)
{
	free(handle);
}

/* --- Accept new connection --- */

/** @brief libuv connection callback — accept a new incoming TCP connection.
 *
 * This is the entry point for every new TCP connection. The sequence is:
 *
 *   1. Connection limiter: if active_connections >= max_connections, accept
 *      and immediately close (drains the kernel backlog without processing).
 *
 *   2. Client acquisition: get a client struct from the pool (pool_get).
 *      Pool reuse avoids calloc/free churn for every connection.
 *
 *   3. TCP handle init: uv_tcp_init + uv_accept to attach the fd.
 *      TCP_NODELAY is applied if configured (disables Nagle's algorithm).
 *
 *   4. Counters: atomic_fetch_add active_connections.
 *
 *   5. Parser init: llhttp_init with the server's callback table.
 *      The parser state machine is reset for each new connection.
 *
 *   6. TLS setup: if ssl_ctx is configured, set up BIO pairs and start
 *      the TLS handshake (setup_client_tls).
 *
 *   7. Timer setup: read_timeout and request_timeout are one-shot timers
 *      that fire if no data arrives within the configured window.
 *
 *   8. Arena init: per-connection bump allocator for request-scoped
 *      allocations (path strings, query params, arena handler chains).
 *
 *   9. Read: uv_read_start registers the on_read callback with libuv.
 *
 * If any step fails (allocation, accept, init), the client is cleaned up
 * via close callbacks and returned to the pool.
 *
 * @param server_stream The listening server stream.
 * @param status        Connection status (negative on error). */
void
on_new_connection(uv_stream_t* server_stream, int status)
{
	if (status < 0) {
		fprintf(stderr, "New connection error %s\n", uv_strerror(status));
		return;
	}

	csilk_server_t* server = (csilk_server_t*)server_stream->data;

	int max_conn = server->config.max_connections;
	if (max_conn == 0) {
		max_conn = server->max_connections;
	}
	if (max_conn > 0 && atomic_load(&server->active_connections) >= max_conn) {
		uv_tcp_t* tmp = malloc(sizeof(uv_tcp_t));
		if (tmp) {
			uv_tcp_init(server_stream->loop, tmp);
			if (uv_accept(server_stream, (uv_stream_t*)tmp) == 0) {
				uv_close((uv_handle_t*)tmp, on_rejected_close);
			} else {
				uv_close((uv_handle_t*)tmp, on_rejected_close);
			}
		}
		return;
	}

	csilk_client_t* client = pool_get(server);
	if (!client) {
		uv_tcp_t* tmp = malloc(sizeof(uv_tcp_t));
		if (tmp) {
			uv_tcp_init(server_stream->loop, tmp);
			if (uv_accept(server_stream, (uv_stream_t*)tmp) == 0) {
				uv_close((uv_handle_t*)tmp, on_rejected_close);
			} else {
				uv_close((uv_handle_t*)tmp, on_rejected_close);
			}
		}
		return;
	}

	client->server = server;
	int r = uv_tcp_init(server_stream->loop, &client->handle);
	if (r < 0) {
		fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
		pool_put(server, client);
		return;
	}
	client->handle.data = client;

	_csilk_ctx_init(&client->ctx, server, client);

	client_list_add(server, client);

	if (uv_accept(server_stream, (uv_stream_t*)&client->handle) == 0) {
		if (server->config.tcp_nodelay) {
			uv_tcp_nodelay((uv_tcp_t*)&client->handle, 1);
		}
		atomic_fetch_add(&server->active_connections, 1);
		llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
		client->parser.data = client;

		_csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

		if (server->ssl_ctx) {
			if (setup_client_tls(client) < 0) {
				uv_close((uv_handle_t*)&client->handle, on_close);
				return;
			}
		}

		uv_timer_init(server_stream->loop, &client->timer);
		client->timer.data = client;
		uv_timer_init(server_stream->loop, &client->read_timer);
		client->read_timer.data = client;
		uv_timer_init(server_stream->loop, &client->write_timer);
		client->write_timer.data = client;
		uv_timer_init(server_stream->loop, &client->request_timer);
		client->request_timer.data = client;

		if (server->config.read_timeout_ms > 0) {
			uv_timer_start(&client->read_timer,
				       on_read_timeout,
				       server->config.read_timeout_ms,
				       0);
		}
		if (server->config.request_timeout_ms > 0) {
			uv_timer_start(&client->request_timer,
				       on_read_timeout,
				       server->config.request_timeout_ms,
				       0);
		}

		client->ctx.arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);

		r = uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
		if (r < 0) {
			fprintf(stderr, "uv_read_start error %s\n", uv_strerror(r));
			if (!uv_is_closing((uv_handle_t*)&client->handle)) {
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
		}
	} else {
		if (!uv_is_closing((uv_handle_t*)&client->handle)) {
			uv_close((uv_handle_t*)&client->handle, on_close);
		}
	}
}

/* --- TCP read --- */

/** @brief libuv read callback — processes incoming data from a client
 * connection.
 *
 * This is the heart of the event-driven I/O model. Every byte from every
 * connection arrives here. The dispatch logic has three paths:
 *
 *   TLS path (client->ssl is set):
 *     Data is written to the read BIO, then process_tls_read() drives the
 *     TLS handshake (if not yet complete) or decrypts and feeds the result
 *     to the llhttp parser (or WebSocket frame parser). Encrypted output
 *     from the write BIO is flushed via flush_tls_write().
 *
 *   WebSocket path (client->ctx.is_websocket):
 *     Data is parsed directly as WebSocket frames by csilk_ws_parse_frame().
 *     No HTTP parsing occurs on this connection after the upgrade.
 *
 *   HTTP path (default):
 *     Data is fed directly to llhttp_execute(). The callbacks in
 *     server->settings (on_url, on_header_field, on_body, etc.) incrementally
 *     build the request struct. When the request is complete,
 *     on_message_complete fires to dispatch routing.
 *
 * On positive nread: feed data to the appropriate handler.
 * On nread == UV_EOF: peer closed the connection; close the client.
 * On nread < 0 (error): log and close.
 *
 * The idle timer is always stopped when data arrives (keep-alive wait
 * is reset). The read timeout is restarted.
 *
 * @param stream The client TCP stream.
 * @param nread  Number of bytes read (negative for error/EOF).
 * @param buf    The buffer that was read into (freed by this callback). */
void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	csilk_client_t* client = (csilk_client_t*)stream->data;
	uv_timer_stop(&client->timer);
	if (client->server->config.read_timeout_ms > 0) {
		uv_timer_start(&client->read_timer,
			       on_read_timeout,
			       client->server->config.read_timeout_ms,
			       0);
	}
	if (nread > 0) {
		if (client->ssl) {
			BIO_write(client->read_bio, buf->base, (int)nread);
			process_tls_read(client);
		} else if (client->ctx.is_websocket) {
			csilk_ws_parse_frame(
			    &client->ctx, (const uint8_t*)buf->base, (size_t)nread);
		} else {
			enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);
			if (err == HPE_CLOSED_CONNECTION) {
				llhttp_init(
				    &client->parser, HTTP_REQUEST, &client->server->settings);
				client->parser.data = client;
			} else if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
				fprintf(stderr,
					"Parse error: %s %s\n",
					llhttp_errno_name(err),
					client->parser.reason);

				if (!uv_is_closing((uv_handle_t*)stream)) {
					uv_close((uv_handle_t*)stream, on_close);
				}
			}
		}
	} else if (nread < 0) {
		if (nread != UV_EOF) {
			fprintf(stderr, "Read error %s\n", uv_err_name((int)nread));
		}
		if (!uv_is_closing((uv_handle_t*)stream)) {
			uv_close((uv_handle_t*)stream, on_close);
		}
	}

	if (buf->base) {
		free(buf->base);
	}
}

/* --- Get client IP --- */

/** @brief Get the remote client's IP address as a string.
 *
 * Resolves the client's IP address (IPv4 or IPv6) from the underlying TCP
 * socket using libuv's getpeername. The result is allocated in arena memory
 * so it is valid for the duration of the request.
 *
 * @param c The request context.
 * @return A string with the client IP (e.g., "127.0.0.1" or "::1"), or NULL
 *         if the context is NULL or the address cannot be resolved. */
const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
	if (!c || !c->_internal_client) {
		return NULL;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	struct sockaddr_storage addr;
	int len = sizeof(addr);
	if (uv_tcp_getpeername(&client->handle, (struct sockaddr*)&addr, &len) == 0) {
		char ip[46];
		if (addr.ss_family == AF_INET) {
			uv_ip4_name((struct sockaddr_in*)&addr, ip, sizeof(ip));
		} else {
			uv_ip6_name((struct sockaddr_in6*)&addr, ip, sizeof(ip));
		}
		return csilk_arena_strdup(c->arena, ip);
	}
	return NULL;
}
