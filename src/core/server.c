/**
 * @file server.c
 * @brief Core event-driven HTTP server implementation.
 *
 * Architecture overview:
 *
 *   This file implements the full lifecycle of a csilk HTTP/TLS server.
 *   The design is built on three layers:
 *
 *   1. Transport layer (libuv): Asynchronous TCP I/O with epoll/kqueue/IO-
 *      completion-ports. All I/O is non-blocking and event-driven.
 *
 *   2. Protocol layer (llhttp): Fast HTTP/1.1 request parsing. Each client
 *      connection has a dedicated llhttp parser instance. Parsing happens
 *      incrementally as data arrives in on_read().
 *
 *   3. Connection lifecycle:
 *        accept (on_new_connection)
 *          -> TLS handshake (if SSL)
 *          -> HTTP parse (on_read -> llhttp)
 *          -> request route (on_message_complete -> router)
 *          -> response (_csilk_send_response)
 *          -> keep-alive or close
 *
 *   Key design decisions:
 *     - TLS uses BIO pairs (memory BIOs) so encryption/decryption is driven
 *       by the same on_read callback without changing the I/O model.
 *     - Client objects are pooled (up to 32) to reduce allocation churn.
 *     - Connection limits are enforced by accept+immediate-close (drains the
 *       kernel backlog without blocking).
 *     - Graceful shutdown is async: csilk_server_stop() sends an async signal
 *       to the event loop, which closes the listener and all active clients.
 *     - Multi-worker mode uses SO_REUSEPORT so each thread has its own accept
 *       loop; the kernel distributes connections across workers.
 *
 * @copyright MIT License
 */

#include <limits.h>
#include <llhttp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Default idle timeout in milliseconds. */
#define CSILK_DEFAULT_IDLE_TIMEOUT 5000
/** @brief Default maximum request body size in bytes. */
#define CSILK_DEFAULT_MAX_BODY_SIZE (1024UL * 1024UL)
/** @brief Default maximum request header size in bytes. */
#define CSILK_DEFAULT_MAX_HEADER_SIZE (64UL * 1024UL)
/** @brief Default TCP listen backlog. */
#define CSILK_DEFAULT_LISTEN_BACKLOG 128
/** @brief Default request arena chunk size. */
#define CSILK_DEFAULT_ARENA_SIZE 4096

/** @brief Forward declaration for client connection structure. */
typedef struct csilk_client_s csilk_client_t;

/** @brief Hook handler node in a linked list. */
typedef struct csilk_hook_node_s {
	void* handler;
	struct csilk_hook_node_s* next;
} csilk_hook_node_t;

/** @brief Server structure — holds the core server state.
 *
 * Manages the libuv event loop, HTTP listener, configuration, global
 * middleware chain, hook registrations, and client connection pooling.
 * Thread-safe for multi-threaded operation via atomic counters and mutexes.
 */
struct csilk_server_s {
	uv_loop_t* loop;		 /**< libuv event loop. */
	csilk_router_t* router;		 /**< Associated router instance. */
	uv_tcp_t server_handle;		 /**< TCP server handle. */
	uv_signal_t sig_handle;		 /**< SIGINT signal handler. */
	uv_async_t async_handle;	 /**< Async handle for cross-thread wakeup. */
	llhttp_settings_t settings;	 /**< HTTP parser callback settings. */
	csilk_server_config_t config;	 /**< Server configuration. */
	csilk_handler_t middlewares[32]; /**< Global middlewares. */
	int middleware_count;		 /**< Number of global middlewares. */
	int max_connections;		 /**< Max concurrent connections (0=unlimited). */
	atomic_int active_connections;	 /**< Current connection count (atomic). */
	/* close tracking for async shutdown — see csilk_server_free */
	uv_thread_t* worker_tids;		/**< Worker thread IDs (NULL if single-thread). */
	int worker_count;			/**< Number of worker threads created. */
	uv_async_t* worker_stop_async;		/**< Per-worker async handles for graceful stop. */
	int worker_stop_count;			/**< Number of worker_stop_async entries. */
	csilk_handler_t not_found_handler;	/**< Custom 404 handler (NULL = default). */
	char* spa_doc_root;			/**< SPA fallback doc root (NULL = disabled). */
	csilk_storage_driver_t* storage_driver; /**< Context storage driver. */
	csilk_crypto_driver_t* crypto_driver;	/**< Crypto algorithm driver. */
	csilk_cipher_driver_t* cipher_driver;	/**< Cipher algorithm driver. */
	SSL_CTX* ssl_ctx;			/**< OpenSSL context. */
	csilk_mq_t* mq;				/**< Message Queue instance. */
	csilk_hook_node_t* hooks[CSILK_HOOK_COUNT]; /**< Registered hooks. */
	csilk_client_t* active_clients;		    /**< Head of active connections list. */
	uv_mutex_t clients_mutex;		    /**< Mutex for active clients list. */
	csilk_client_t* client_pool[32];	    /**< Connection object free list. */
	int client_pool_count;			    /**< Number of free clients in pool. */
	uv_mutex_t pool_mutex;			    /**< Mutex for connection pool access. */
};

/** @brief Client connection structure — represents a single TCP connection.
 *
 * Holds the libuv stream handle, HTTP parser state, timers for keep-alive
 * and timeouts, TLS context (if HTTPS), and the request/response context.
 * Clients are pooled and reused for performance.
 */
struct csilk_client_s {
	uv_tcp_t handle;	      /**< libuv TCP stream handle. */
	uv_timer_t timer;	      /**< Connection idle (keep-alive) timer. */
	uv_timer_t read_timer;	      /**< Read timeout timer. */
	uv_timer_t write_timer;	      /**< Write timeout timer. */
	uv_timer_t request_timer;     /**< Request timeout timer. */
	int close_pending;	      /**< Pending close refs before freeing client. */
	llhttp_t parser;	      /**< HTTP request parser. */
	csilk_server_t* server;	      /**< Owning server instance. */
	csilk_ctx_t ctx;	      /**< Request context for this connection. */
	size_t total_header_size;     /**< Total size of headers parsed so far. */
	size_t header_count;	      /**< Number of headers parsed so far. */
	size_t current_url_capacity;  /**< Allocated size of current_url. */
	size_t header_field_capacity; /**< Allocated size of current_header_field. */
	size_t header_value_capacity; /**< Allocated size of current_header_value. */
	char* current_url;	      /**< Current URL being parsed. */
	char* current_header_field;   /**< Temporary header field name. */
	char* current_header_value;   /**< Temporary header field value. */
	SSL* ssl;		      /**< OpenSSL session object. */
	BIO* read_bio;		      /**< BIO for reading encrypted data. */
	BIO* write_bio;		      /**< BIO for writing encrypted data. */
	struct csilk_client_s* next;  /**< Next client in active list. */
	struct csilk_client_s* prev;  /**< Previous client in active list. */
};

/** @brief libuv buffer allocation callback — allocates a receive buffer.
 *
 * Allocates a buffer of the suggested size using malloc. The buffer is freed
 * by libuv after the read callback is invoked.
 *
 * @param handle          The libuv handle that will read into the buffer.
 * @param suggested_size  Recommended buffer size from libuv.
 * @param buf             [out] Pointer to the uv_buf_t to populate. */
static void
alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	(void)handle;
	buf->base = (char*)malloc(suggested_size);
	buf->len = suggested_size;
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void on_idle_timeout(uv_timer_t* handle);
static void on_write_timeout(uv_timer_t* handle);
static void on_server_handle_close(uv_handle_t* handle);
static void init_tls(csilk_server_t* s);
static void cleanup_tls(csilk_server_t* s);
static int setup_client_tls(csilk_client_t* client);
static void process_tls_read(csilk_client_t* client);
static void flush_tls_write(csilk_client_t* client);
static void trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type);

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
		client->read_bio = NULL;  // Frees with SSL_free
		client->write_bio = NULL; // Frees with SSL_free
	}
	memset(client, 0, sizeof(*client));
	uv_mutex_lock(&server->pool_mutex);
	if (server->client_pool_count < 32) {
		server->client_pool[server->client_pool_count++] = client;
	} else {
		free(client);
	}
	uv_mutex_unlock(&server->pool_mutex);
}

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
static void
on_close(uv_handle_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (client) {
		trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
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

/** @brief libuv signal handler for SIGINT — initiates graceful server shutdown.
 *
 * Delegates to csilk_server_stop() which sends an async signal to the
 * event loop to trigger cleanup on the main loop thread.
 *
 * @param handle libuv signal handle (data points to csilk_server_t).
 * @param signum Received signal number (e.g., SIGINT). */
static void
on_signal(uv_signal_t* handle, int signum)
{
	(void)signum;
	csilk_server_t* server = (csilk_server_t*)handle->data;
	csilk_server_stop(server);
}

/** @brief libuv async callback to stop the server gracefully.
 *
 * This function performs the full shutdown sequence:
 *
 *   1. Fire CSILK_HOOK_SERVER_STOP — so users can flush state.
 *   2. Close the listener (uv_close) — stops accepting new connections.
 *   3. Iterate active_clients and close each connection appropriately:
 *        - WebSocket: send close frame (1001) to notify the peer.
 *        - SSE: send a "close" event, then close the connection.
 *        - HTTP: close immediately (existing requests finish via on_close).
 *      ONLY main loop handles are closed here.
 *   4. Close the SIGINT and async handles.
 *   5. Signal all worker threads to stop.
 *   6. Free the message queue.
 *
 * The actual client struct cleanup happens asynchronously in on_close()
 * when each TCP handle finishes closing. This avoids blocking the event
 * loop for connection draining.
 *
 * @param handle libuv async handle (data points to csilk_server_t). */
static void
on_stop_async(uv_async_t* handle)
{
	csilk_server_t* server = (csilk_server_t*)handle->data;

	trigger_hooks(server, NULL, CSILK_HOOK_SERVER_STOP);

	// Close the server listen handle (stop accepting new connections)
	if (!uv_is_closing((uv_handle_t*)&server->server_handle)) {
		uv_close((uv_handle_t*)&server->server_handle, on_server_handle_close);
	}

	// Signal all active clients belonging to THIS thread's loop
	uv_mutex_lock(&server->clients_mutex);
	csilk_client_t* client = server->active_clients;
	while (client) {
		if (client->handle.loop == server->loop) {
			if (client->ctx.is_websocket) {
				csilk_ws_close(&client->ctx, 1001, "Server stopping");
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
			} else if (client->ctx.is_sse) {
				csilk_sse_send(&client->ctx, "close", "Server stopping");
				csilk_sse_close(&client->ctx);
			} else {
				// Normal HTTP: just close if not already closing
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
			}
		}
		client = client->next;
	}
	uv_mutex_unlock(&server->clients_mutex);

	// Close signal and async handles
	if (!uv_is_closing((uv_handle_t*)&server->sig_handle)) {
		uv_close((uv_handle_t*)&server->sig_handle, on_server_handle_close);
	}
	if (!uv_is_closing((uv_handle_t*)&server->async_handle)) {
		uv_close((uv_handle_t*)&server->async_handle, on_server_handle_close);
	}

	// Signal all worker threads to stop
	for (int i = 0; i < server->worker_stop_count; i++) {
		uv_async_send(&server->worker_stop_async[i]);
	}

	// Close MQ async handle
	if (server->mq) {
		_csilk_mq_free(server->mq);
		server->mq = NULL;
	}
}

/** @brief libuv sendfile completion callback — continues with keep-alive or
 * close.
 *
 * sendfile() is used for efficient zero-copy file serving (kernel copies
 * file data directly to the socket without userspace buffering). After
 * sendfile completes, this callback:
 *
 *   1. Cleans up the uv_fs_t request (freed).
 *   2. Checks whether the connection should be kept alive (HTTP/1.1
 *      Connection header).
 *   3. If keep-alive: restarts the idle timer and begins reading again.
 *   4. If close: initiates uv_close to tear down the connection.
 *   5. Fires CSILK_HOOK_REQUEST_END and cleans up the request context.
 *
 * sendfile is only used for non-TLS connections; TLS connections must
 * use the buffered write path (SSL_write) because file data must be
 * encrypted before transmission.
 *
 * @param req The completed uv_fs_t sendfile request (freed by this callback).
 */
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

	/* Finalize request handling (duplicated from _csilk_send_response) */
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

	trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);
	csilk_ctx_cleanup(&client->ctx);
}

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
static void
on_write(uv_write_t* req, int status)
{
	if (status < 0) {
		fprintf(stderr, "Write error %s\n", uv_strerror(status));
	}
	// Stop write timeout (response flushed)
	csilk_client_t* client = NULL;
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
				client->ctx.file_fd = -1; /* Prevent recursion */

				int r = uv_fs_sendfile(uv_default_loop(),
						       fs_req,
						       sock_fd,
						       fd,
						       offset,
						       size,
						       on_sendfile_complete);
				if (r < 0) {
					/* Fallback or error */
					free(fs_req);
					/* Should we close? */
				} else {
					free(req);
					return; /* Wait for sendfile to complete before freeing context */
				}
			}
		}
	}

	free(req);
}

/** @brief libuv timer callback: fired when the connection has been idle
 * (no active request) beyond keepalive_idle_ms.
 *
 * Closes the client connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data). */
static void
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
static void
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
static void
on_write_timeout(uv_timer_t* handle)
{
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (!uv_is_closing((uv_handle_t*)&client->handle)) {
		uv_close((uv_handle_t*)&client->handle, on_close);
	}
}

/** @brief llhttp callback: a new HTTP message begins.
 *
 * Resets per-request state (total_header_size, header_count, etc. are reset
 * elsewhere). Clears the thread-local request ID so each request starts fresh.
 * Stops and restarts the request timeout timer.
 *
 * @param p The llhttp parser instance (data points to csilk_client_t).
 * @return 0 (HPE_OK) to continue parsing. */
static int
on_message_begin(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size = 0;
	client->header_count = 0;

	// Restart request timeout for this new request (keep-alive)
	if (client->server->config.request_timeout_ms > 0) {
		uv_timer_stop(&client->request_timer);
		uv_timer_start(&client->request_timer,
			       on_read_timeout,
			       client->server->config.request_timeout_ms,
			       0);
	}

	csilk_log_set_request_id(NULL);
	return 0;
}

/** @brief llhttp callback: URL data received.
 *
 * Stores the raw URL string. Checks against max_url_size and returns
 * HPE_USER if exceeded (aborts parsing).
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to the URL data.
 * @param length Length of the URL data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if URL exceeds max_url_size. */
static int
on_url(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	size_t max_url = client->server->config.max_url_size;
	if (max_url > 0 && length > max_url) {
		fprintf(stderr, "[debug] on_url max_url exceeded\n");
		return HPE_USER;
	}
	if (client->current_url) {
		free(client->current_url);
	}
	client->current_url = malloc(length + 1);
	if (!client->current_url) {
		fprintf(stderr, "[debug] on_url malloc failed\n");
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
static int
on_header_field(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size += length;
	if (client->total_header_size > client->server->config.max_header_size) {
		fprintf(stderr, "[debug] on_header_field max_header_size exceeded\n");
		return HPE_USER;
	}
	client->header_count++;
	if (client->server->config.max_headers_count > 0 &&
	    client->header_count > client->server->config.max_headers_count) {
		fprintf(stderr, "[debug] on_header_field max_headers_count exceeded\n");
		return HPE_USER;
	}

	if (client->current_header_field && client->current_header_value) {
		csilk_set_request_header(
		    &client->ctx, client->current_header_field, client->current_header_value);
		free(client->current_header_field);
		client->current_header_field = NULL;
		client->header_field_capacity = 0;
		free(client->current_header_value);
		client->current_header_value = NULL;
		client->header_value_capacity = 0;
	} else if (client->current_header_field) {
		free(client->current_header_field);
		client->current_header_field = NULL;
		client->header_field_capacity = 0;
	}

	client->current_header_field = malloc(length + 1);
	if (!client->current_header_field) {
		fprintf(stderr, "[debug] on_header_field malloc failed\n");
		return HPE_USER;
	}
	memcpy(client->current_header_field, at, length);
	client->current_header_field[length] = '\0';
	return 0;
}

/** @brief Grow a heap-allocated buffer to at least @p needed bytes.
 *
 * Uses realloc with capacity doubling for amortized O(1) growth. If @p buf
 * is NULL and *@p cap is 0, this acts as a malloc. On realloc failure the
 * original buffer is NOT freed (caller must free it).
 *
 * @param buf    Existing allocation (may be NULL).
 * @param cap    [in,out] Current capacity — updated on success.
 * @param needed Minimum required size in bytes.
 * @return Pointer to the resized buffer, or NULL on allocation failure. */
static char*
buf_grow(char* buf, size_t* cap, size_t needed)
{
	if (needed <= *cap) {
		return buf;
	}
	size_t new_cap = *cap ? *cap : 32;
	while (new_cap < needed) {
		new_cap *= 2;
	}
	char* new_buf = realloc(buf, new_cap);
	if (!new_buf) {
		return NULL;
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
static int
on_header_value(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	client->total_header_size += length;
	if (client->total_header_size > client->server->config.max_header_size) {
		fprintf(stderr, "[debug] on_header_value max_header_size exceeded\n");
		return HPE_USER;
	}

	size_t prev_len = client->current_header_value ? strlen(client->current_header_value) : 0;
	size_t needed = prev_len + length + 1;
	char* new_val =
	    buf_grow(client->current_header_value, &client->header_value_capacity, needed);
	if (!new_val) {
		free(client->current_header_value);
		client->current_header_value = NULL;
		client->header_value_capacity = 0;
		client->total_header_size = 0;
		fprintf(stderr, "[debug] on_header_value realloc failed\n");
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
static int
on_headers_complete(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	if (client->current_header_field && client->current_header_value) {
		csilk_set_request_header(
		    &client->ctx, client->current_header_field, client->current_header_value);
		free(client->current_header_field);
		client->current_header_field = NULL;
		client->header_field_capacity = 0;
		free(client->current_header_value);
		client->current_header_value = NULL;
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
static int
on_body(llhttp_t* p, const char* at, size_t length)
{
	csilk_client_t* client = (csilk_client_t*)p->data;
	if (client->ctx.request.body_len + length > client->server->config.max_body_size) {
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
		client->ctx.request.body = NULL;
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

/** @brief Send raw data to the client (TLS-aware).
 *
 * If TLS is active, writes through the SSL session and flushes the write BIO.
 * Otherwise, allocates a write request, copies the data, and queues the write
 * via libuv. The data buffer is freed by the write completion callback.
 *
 * @param c    The request context.
 * @param data Data buffer to send.
 * @param len  Length of data in bytes.
 * @note This is an internal function used by the framework to send HTTP
 *       responses, chunked frames, and WebSocket frames. */
void
_csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
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
void
_csilk_send_response(csilk_ctx_t* c)
{
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client) {
		return;
	}

	// Stop request timeout timer (response is being sent)
	uv_timer_stop(&client->request_timer);

	int status = client->ctx.response.status ? client->ctx.response.status : 200;
	const char* status_text = get_status_text(status);

	// Determine response mode:
	//   is_file   = true when a file descriptor is available AND the
	//               connection is not TLS (sendfile does not work with TLS).
	//   use_chunked = true when the body is empty AND the handler has
	//               set is_async (the caller will stream chunks later).
	//   Otherwise, use Content-Length with the body inline.
	int is_file = (c->file_fd >= 0 && !client->ssl);
	int use_chunked = (client->ctx.response.body_len == 0 && client->ctx.is_async && !is_file);
	const char* transfer_encoding = use_chunked ? "Transfer-Encoding: chunked\r\n" : "";

	// Calculate total size of custom response headers (for buffer allocation)
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
		header_len = snprintf(NULL, 0, "HTTP/1.1 101 Switching Protocols\r\n");
	} else if (use_chunked) {
		header_len = snprintf(NULL,
				      0,
				      "HTTP/1.1 %d %s\r\n"
				      "%s"
				      "Connection: %s\r\n",
				      status,
				      status_text,
				      transfer_encoding,
				      connection_val);
	} else {
		header_len = snprintf(NULL,
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

	// For non-chunked response, the length should be header + headers + crlf +
	// body
	size_t response_len =
	    (size_t)header_len + custom_headers_len + 2 + (use_chunked || is_file ? 0 : body_len);

	char* write_base = malloc(response_len + 1);
	if (write_base) {
		int pos;
		if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
			pos = snprintf(
			    write_base, response_len + 1, "HTTP/1.1 101 Switching Protocols\r\n");
		} else if (use_chunked) {
			pos = snprintf(write_base,
				       response_len + 1,
				       "HTTP/1.1 %d %s\r\n"
				       "%s"
				       "Connection: %s\r\n",
				       status,
				       status_text,
				       transfer_encoding,
				       connection_val);
		} else {
			pos = snprintf(write_base,
				       response_len + 1,
				       "HTTP/1.1 %d %s\r\n"
				       "Content-Length: %zu\r\n"
				       "Connection: %s\r\n",
				       status,
				       status_text,
				       body_len,
				       connection_val);
		}

		for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
			for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h;
			     h = h->next) {
				memcpy(write_base + pos, h->key, h->key_len);
				pos += (int)h->key_len;
				write_base[pos++] = ':';
				write_base[pos++] = ' ';
				memcpy(write_base + pos, h->value, h->value_len);
				pos += (int)h->value_len;
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
		/* Defer finalization to on_sendfile_complete */
		return;
	}

	// Stop read timeout (request is complete)
	uv_timer_stop(&client->read_timer);

	// Start write timeout
	if (client->server->config.write_timeout_ms > 0) {
		uv_timer_start(&client->write_timer,
			       on_write_timeout,
			       client->server->config.write_timeout_ms,
			       0);
	}

	if (client->ctx.is_websocket) {
		// WebSocket or SSE connection: keep alive without idle timer
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

	trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);

	csilk_ctx_cleanup(&client->ctx);
}

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
		client->current_header_field = NULL;
		client->header_field_capacity = 0;
		free(client->current_header_value);
		client->current_header_value = NULL;
		client->header_value_capacity = 0;
	}

	if (client->current_url) {
		char* path = NULL;
		char* query = NULL;
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
		client->current_url = NULL;
	}

	client->ctx.request.method = (char*)llhttp_method_name(llhttp_get_method(p));
}

/** @brief llhttp callback: the full HTTP request message has been parsed.
 *
 * This is the main request dispatch point. It executes the following
 * pipeline for every incoming HTTP request:
 *
 *   1. finalize_request(): store remaining headers, split URL into path
 *      and query, URL-decode the path, parse query parameters.
 *
 *   2. trigger_hooks(CSILK_HOOK_REQUEST_BEGIN): user-registered
 *      request-start hooks (e.g., request logging, rate limiting).
 *
 *   3. csilk_router_match_ctx(): walk the radix tree to find a matching
 *      route. If matched, the handler chain is set on ctx->handlers.
 *
 *   4. Global middleware prepend: if the server has global middlewares
 *      (registered via csilk_server_use()), they are prepended to the
 *      route-specific handler chain. The combined chain is allocated from
 *      the arena (no heap fragmentation for per-request allocations).
 *
 *   5. csilk_next(): execute the handler chain. Handlers call csilk_next()
 *      to pass to the next handler, or send a response and mark is_async.
 *
 *   6. If no route matched: invoke the 404 handler or send a default
 *      "Not Found" plain-text response.
 *
 *   7. If not async: send the response synchronously via
 *      _csilk_send_response(). For async handlers, the response is sent
 *      later when the handler decides to complete.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) on success, non-zero to abort parsing. */
static int
on_message_complete(llhttp_t* p)
{
	csilk_client_t* client = (csilk_client_t*)p->data;

	finalize_request(client, p);
	CSILK_LOG_I("Request: %s %s", client->ctx.request.method, client->ctx.request.path);

	trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_BEGIN);

	if (csilk_router_match_ctx(client->server->router, &client->ctx)) {
		CSILK_LOG_D("Route matched, calling next handler");

		// Prepend global middlewares
		if (client->server->middleware_count > 0) {
			int route_handler_count = 0;
			while (client->ctx.handlers[route_handler_count] != NULL) {
				route_handler_count++;
			}

			int total_count = client->server->middleware_count + route_handler_count;
			csilk_handler_t* arena_handlers = csilk_arena_alloc(
			    client->ctx.arena, (total_count + 1) * sizeof(csilk_handler_t));
			if (arena_handlers) {
				for (int i = 0; i < client->server->middleware_count; i++) {
					arena_handlers[i] = client->server->middlewares[i];
				}
				for (int i = 0; i < route_handler_count; i++) {
					arena_handlers[client->server->middleware_count + i] =
					    client->ctx.handlers[i];
				}
				arena_handlers[total_count] = NULL;
				client->ctx.handlers = arena_handlers;
			}
		}

		csilk_next(&client->ctx);
	} else {
		CSILK_LOG_W("Route not found: %s", client->ctx.request.path);
		if (client->server->not_found_handler) {
			client->server->not_found_handler(&client->ctx);
		} else {
			csilk_string(&client->ctx, CSILK_STATUS_NOT_FOUND, "Not Found");
		}
	}

	if (client->ctx.is_async) {
		uv_read_stop((uv_stream_t*)&client->handle);
	}

	if (!client->ctx.is_async) {
		_csilk_send_response(&client->ctx);
	}
	return 0;
}

static void
on_rejected_close(uv_handle_t* handle)
{
	free(handle);
}

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
static void
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
		/* accept and immediately close to drain the backlog.
		 * MUST heap-allocate the handle because uv_close is async and
		 * stack-allocated handle would go out of scope. */
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
		/* accept and close if we can't get a client object (OOM).
		 * Same logic as connection limiter above. */
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
	client->ctx._internal_client = client;
	client->ctx.storage_driver = server->storage_driver;
	client->ctx.crypto_driver = server->crypto_driver;
	client->ctx.cipher_driver = server->cipher_driver;
	client_list_add(server, client);

	if (uv_accept(server_stream, (uv_stream_t*)&client->handle) == 0) {
		if (server->config.tcp_nodelay) {
			uv_tcp_nodelay((uv_tcp_t*)&client->handle, 1);
		}
		atomic_fetch_add(&server->active_connections, 1);
		llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
		client->parser.data = client;

		trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

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

		// Start read timeout and request timeout timers
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

		// Initialize arena for the request
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
static void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	csilk_client_t* client = (csilk_client_t*)stream->data;
	uv_timer_stop(&client->timer);
	// Reset read timeout on data arrival
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

#include "csilk/reflection/reflect.h"

/** @brief Create a new server instance associated with a router.
 *
 * Initializes the reflection system, allocates the server struct, sets up
 * the libuv default loop, configures the llhttp parser callbacks, applies
 * default server configuration (timeouts, buffer limits, backlog), creates
 * a clients mutex, and creates the internal message queue (MQ) instance.
 *
 * @param router The router instance to use for request matching.
 * @return A new csilk_server_t instance, or NULL on allocation failure.
 * @note The server must be configured (via csilk_server_set_config()) and
 *       started via csilk_server_run(). Free with csilk_server_free(). */
csilk_server_t*
csilk_server_new(csilk_router_t* router)
{
	csilk_reflect_init();
	csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
	if (!s) {
		return NULL;
	}
	s->loop = uv_default_loop();
	if (!s->loop) {
		free(s);
		return NULL;
	}
	s->router = router;
	llhttp_settings_init(&s->settings);
	s->settings.on_message_begin = on_message_begin;
	s->settings.on_url = on_url;
	s->settings.on_header_field = on_header_field;
	s->settings.on_header_value = on_header_value;
	s->settings.on_headers_complete = on_headers_complete;
	s->settings.on_body = on_body;
	s->settings.on_message_complete = on_message_complete;

	s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
	s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
	s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
	s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;

	uv_mutex_init(&s->clients_mutex);
	uv_mutex_init(&s->pool_mutex);

	s->mq = _csilk_mq_new(s->loop);

	return s;
}

/** @brief Built-in SPA (Single Page Application) fallback handler.
 *
 * For unmatched GET requests, attempts to serve "index.html" from the
 * configured SPA doc root. This enables client-side routing for SPAs like
 * React, Vue, or Angular that handle their own URL routing in the browser.
 *
 * @param c The request context.
 * @note Only applies to GET requests. Non-GET unmatched requests receive a
 *       standard 404 response. The doc root is set via
 *       csilk_server_set_spa_fallback(). */
static void
spa_fallback_handler(csilk_ctx_t* c)
{
	const char* method = csilk_get_method(c);
	if (!method || strcmp(method, "GET") != 0) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client || !client->server || !client->server->spa_doc_root) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/index.html", client->server->spa_doc_root);

	FILE* f = fopen(path, "rb");
	if (!f) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	if (fsize <= 0) {
		fclose(f);
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	rewind(f);
	char* body = malloc((size_t)fsize + 1);
	if (!body) {
		fclose(f);
		csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
		return;
	}
	size_t nread = fread(body, 1, (size_t)fsize, f);
	fclose(f);
	body[nread] = '\0';

	csilk_set_header(c, "Content-Type", "text/html");
	c->response.body = body;
	c->response.body_len = nread;
	c->response.body_is_managed = 1;
	csilk_status(c, CSILK_STATUS_OK);
}

/** @brief Set a custom handler for unmatched routes (404 Not Found).
 *
 * Replaces the default "Not Found" plain-text response with a custom handler.
 * Overridden by csilk_server_set_spa_fallback().
 *
 * @param server  The server instance.
 * @param handler Handler function invoked for unmatched routes. */
void
csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler)
{
	if (!server) {
		return;
	}
	server->not_found_handler = handler;
}

/** @brief Enable SPA fallback: all unmatched GET requests serve index.html from
 * the given directory.
 *
 * Sets the SPA document root and replaces the 404 handler with the built-in
 * spa_fallback_handler. Overrides any custom 404 handler set via
 * csilk_server_set_not_found_handler().
 *
 * @param server   The server instance.
 * @param doc_root Absolute or relative filesystem path to the directory
 *                 containing index.html.
 * @note The doc_root string is strdup'd internally. Pass NULL to disable. */
void
csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root)
{
	if (!server || !doc_root) {
		return;
	}
	free(server->spa_doc_root);
	server->spa_doc_root = strdup(doc_root);
	if (server->spa_doc_root) {
		server->not_found_handler = spa_fallback_handler;
	}
}

/** @brief Register a global middleware handler that runs before every request.
 *
 * Global middlewares are prepended to the matched route's handler chain.
 * They run before route-specific middleware and the final handler. There
 * is a hard limit of 32 global middlewares.
 *
 * @param server  The server instance.
 * @param handler Middleware handler function.
 * @return 0 on success, -1 if the limit is reached or parameters are NULL. */
int
csilk_server_use(csilk_server_t* server, csilk_handler_t handler)
{
	if (!server || !handler) {
		return -1;
	}
	if (server->middleware_count >= 32) {
		CSILK_LOG_E("Global middleware limit (32) reached. Middleware "
			    "dropped.");
		return -1;
	}
	server->middlewares[server->middleware_count++] = handler;
	return 0;
}

/** @brief libuv close callback for server-level handles during shutdown.
 *
 * Currently a no-op placeholder. Called when the server, signal, or async
 * handles finish closing.
 *
 * @param handle The handle being closed (unused). */
static void
on_server_handle_close(uv_handle_t* handle)
{
	(void)handle;
}

/** @brief Free a server instance and all associated resources.
 *
 * Should only be called after the event loop has stopped. Joins any worker
 * threads, frees the SPA doc root, drains the client pool, cleans up TLS,
 * frees the message queue, frees all registered hooks, destroys the clients
 * mutex, and frees the server struct.
 *
 * @param server The server to free (may be NULL).
 * @note Safe to call with NULL. After this call the server pointer is
 *       invalid. */
void
csilk_server_free(csilk_server_t* server)
{
	if (!server) {
		return;
	}

	// Join worker threads (they will exit when their loops stop)
	if (server->worker_tids) {
		for (int i = 0; i < server->worker_count; i++) {
			uv_thread_join(&server->worker_tids[i]);
		}
		free(server->worker_tids);
		server->worker_tids = NULL;
		free(server->worker_stop_async);
		server->worker_stop_async = NULL;
		server->worker_stop_count = 0;
	}

	free(server->spa_doc_root);
	for (int i = 0; i < server->client_pool_count; i++) {
		free(server->client_pool[i]);
	}

	cleanup_tls(server);

	if (server->mq) {
		_csilk_mq_free(server->mq);
	}

	for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
		csilk_hook_node_t* curr = server->hooks[i];
		while (curr) {
			csilk_hook_node_t* next = curr->next;
			free(curr);
			curr = next;
		}
	}

	uv_mutex_destroy(&server->pool_mutex);
	uv_mutex_destroy(&server->clients_mutex);
	free(server);
}

/** @brief Signal the server to stop gracefully (thread-safe).
 *
 * Sends an async signal to the event loop which triggers on_stop_async() on
 * the main loop thread. The function returns immediately; the server shuts
 * down asynchronously.
 *
 * @param server The server instance.
 * @note This is safe to call from any thread, including signal handlers. */
void
csilk_server_stop(csilk_server_t* server)
{
	if (!server) {
		return;
	}
	uv_async_send(&server->async_handle);
}

void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
	if (!server) {
		return;
	}
	if (active_conn) {
		*active_conn = atomic_load(&server->active_connections);
	}
	if (pooled_conn) {
		*pooled_conn = server->client_pool_count;
	}
}

/** @brief Apply a server configuration struct, overwriting the current
 * settings.
 *
 * Copies the provided configuration into the server instance. This should
 * be called before csilk_server_run().
 *
 * @param server The server instance.
 * @param config Pointer to the configuration to apply (copied by value). */
void
csilk_server_set_config(csilk_server_t* server, const csilk_server_config_t* config)
{
	if (!server || !config) {
		return;
	}

	/* Preserve old config to retain defaults for unprovided (zero) fields */
	csilk_server_config_t old = server->config;

	server->config = *config;

	if (server->config.idle_timeout_ms == 0) {
		server->config.idle_timeout_ms =
		    old.idle_timeout_ms ? old.idle_timeout_ms : CSILK_DEFAULT_IDLE_TIMEOUT;
	}
	if (server->config.max_body_size == 0) {
		server->config.max_body_size =
		    old.max_body_size ? old.max_body_size : CSILK_DEFAULT_MAX_BODY_SIZE;
	}
	if (server->config.max_header_size == 0) {
		server->config.max_header_size =
		    old.max_header_size ? old.max_header_size : CSILK_DEFAULT_MAX_HEADER_SIZE;
	}
	if (server->config.listen_backlog == 0) {
		server->config.listen_backlog =
		    old.listen_backlog ? old.listen_backlog : CSILK_DEFAULT_LISTEN_BACKLOG;
	}
}

/** @brief Set the maximum number of concurrent client connections.
 *
 * When this limit is reached, new connections are accepted and immediately
 * closed to drain the listen backlog. A value of 0 means unlimited.
 *
 * @param server The server instance.
 * @param max    Maximum concurrent connections (0 for unlimited).
 * @return The previous maximum connections value, or -1 if server is NULL. */
int
csilk_server_set_max_connections(csilk_server_t* server, int max)
{
	if (!server) {
		return -1;
	}
	int prev = server->max_connections;
	server->max_connections = max;
	return prev;
}

/** @brief Set the pluggable storage driver for context key-value operations.
 *
 * When set, calls to csilk_set()/csilk_get() on request contexts belonging
 * to this server will delegate to the driver instead of using the default
 * arena-backed linked list.
 *
 * @param server The server instance.
 * @param driver Pointer to the storage driver vtable (may be NULL to reset). */
void
csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver)
{
	if (server) {
		server->storage_driver = driver;
	}
}

/** @brief Set the pluggable cryptographic driver for the server.
 *
 * When set, HMAC and UUID operations on request contexts will delegate to
 * the driver instead of using the built-in software implementations.
 *
 * @param server The server instance.
 * @param driver Pointer to the crypto driver vtable (may be NULL to reset). */
void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
	if (server) {
		server->crypto_driver = driver;
	}
}

void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
	if (server) {
		server->cipher_driver = driver;
	}
}

/** @brief Register a lifecycle hook on the server.
 *
 * Hooks are invoked at specific points in the request lifecycle
 * (conn_open, conn_close, request_begin, request_end, server_start,
 * server_stop). Multiple handlers can be registered for the same hook type;
 * they are called in reverse order of registration (LIFO).
 *
 * @param s       The server instance.
 * @param type    Hook type (CSILK_HOOK_CONN_OPEN, CSILK_HOOK_REQUEST_BEGIN,
 *                CSILK_HOOK_CONN_CLOSE, CSILK_HOOK_REQUEST_END,
 *                CSILK_HOOK_SERVER_START, CSILK_HOOK_SERVER_STOP).
 * @param handler Function pointer. For server hooks (start/stop), the
 *                signature is void(*)(csilk_server_t*). For context hooks,
 *                the signature is void(*)(csilk_ctx_t*). */
void
csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler)
{
	if (!s || type < 0 || type >= CSILK_HOOK_COUNT || !handler) {
		return;
	}

	csilk_hook_node_t* node = malloc(sizeof(csilk_hook_node_t));
	if (!node) {
		return;
	}

	node->handler = handler;
	node->next = s->hooks[type];
	s->hooks[type] = node;
}

/** @brief Internal: invoke all registered handlers for a given hook type.
 *
 * Walks the hook's linked list and calls each handler. Server-level hooks
 * (start/stop) receive the server pointer. Context-level hooks receive the
 * request context pointer.
 *
 * @param s    The server instance.
 * @param c    The request context (may be NULL for server-level hooks).
 * @param type Hook type to trigger. */
static void
trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type)
{
	if (!s || type < 0 || type >= CSILK_HOOK_COUNT) {
		return;
	}

	csilk_hook_node_t* curr = s->hooks[type];
	while (curr) {
		if (type <= CSILK_HOOK_SERVER_STOP) {
			((csilk_server_hook_handler_t)curr->handler)(s);
		} else {
			if (c) {
				((csilk_ctx_hook_handler_t)curr->handler)(c);
			}
		}
		curr = curr->next;
	}
}

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#endif

static int
bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port, int backlog, bool reuseport);

/** @brief Per-worker thread initialization data for SO_REUSEPORT multi-loop
 *         mode.
 *
 * Passed to worker_thread() when spawning multiple accept loops. */
typedef struct {
	csilk_server_t* server;	    /**< The server instance. */
	int port;		    /**< Port to listen on. */
	int worker_index;	    /**< Index into server->worker_stop_async[]. */
	pthread_barrier_t* barrier; /**< Barrier synchronising worker init with main
                                  thread. */
} worker_data_t;

typedef struct {
	uv_loop_t* loop;	 /**< The worker's event loop. */
	uv_tcp_t* listen_handle; /**< The worker's local listen handle. */
	csilk_server_t* server;	 /**< The server instance. */
	int worker_index;	 /**< Index for worker_stop_async. */
} worker_stop_data_t;

/** @brief Async callback for stopping a worker's event loop gracefully.
 *
 * This is the worker-thread equivalent of on_stop_async. It:
 *   1. Closes the worker's local listen handle.
 *   2. Closes all active connections owned by this worker's loop.
 *   3. Closes the stop async handle itself.
 *
 * The loop will then naturally exit when all close callbacks finish.
 *
 * @param handle The per-worker async handle (data points to worker_stop_data_t). */
static void
on_worker_stop_async(uv_async_t* handle)
{
	worker_stop_data_t* sd = (worker_stop_data_t*)handle->data;
	if (!sd) {
		return;
	}

	csilk_server_t* server = sd->server;
	uv_loop_t* loop = sd->loop;

	// 1. Close local listen handle
	if (!uv_is_closing((uv_handle_t*)sd->listen_handle)) {
		uv_close((uv_handle_t*)sd->listen_handle, NULL);
	}

	// 2. Close connections belonging to this worker
	uv_mutex_lock(&server->clients_mutex);
	csilk_client_t* client = server->active_clients;
	while (client) {
		if (client->handle.loop == loop) {
			if (client->ctx.is_websocket) {
				csilk_ws_close(&client->ctx, 1001, "Server stopping");
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
			} else if (client->ctx.is_sse) {
				csilk_sse_send(&client->ctx, "close", "Server stopping");
				csilk_sse_close(&client->ctx);
			} else {
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
			}
		}
		client = client->next;
	}
	uv_mutex_unlock(&server->clients_mutex);

	// 3. Close the stop async handle itself to allow the loop to exit
	if (!uv_is_closing((uv_handle_t*)handle)) {
		uv_close((uv_handle_t*)handle, NULL);
	}
}

/** @brief Worker thread entry point for multi-threaded SO_REUSEPORT mode.
 *
 * Each worker runs its own libuv event loop and accept loop, sharing the
 * same port via SO_REUSEPORT. The kernel distributes incoming connections
 * across worker threads. The worker_data_t argument is freed by this function.
 *
 * The worker registers a per-worker uv_async_t in
 * server->worker_stop_async[idx] so the main thread can signal it to stop
 * gracefully.  After uv_run returns, the async handle and the server_handle
 * are closed synchronously, then the loop is closed.
 *
 * @param arg Pointer to worker_data_t (freed when the function exits). */
static void
worker_thread(void* arg)
{
	worker_data_t* data = (worker_data_t*)arg;
	csilk_server_t* server = data->server;
	int port = data->port;
	int idx = data->worker_index;
	pthread_barrier_t* barrier = data->barrier;
	free(data);

	uv_loop_t loop;
	uv_loop_init(&loop);

	uv_tcp_t server_handle;
	server_handle.data = server;

	if (bind_and_listen(&loop, &server_handle, port, server->config.listen_backlog, true) < 0) {
		if (barrier) {
			pthread_barrier_wait(barrier);
		}
		uv_loop_close(&loop);
		return;
	}

	/* Register a per-worker async handle so the main thread can stop us */
	worker_stop_data_t sd = {&loop, &server_handle, server, idx};
	server->worker_stop_async[idx].data = &sd;
	uv_async_init(&loop, &server->worker_stop_async[idx], on_worker_stop_async);

	/* Signal the main thread that this worker is ready */
	if (barrier) {
		pthread_barrier_wait(barrier);
	}

	uv_run(&loop, UV_RUN_DEFAULT);

	uv_loop_close(&loop);
}

// UV_HANDLE_BOUND lives in libuv's private uv-common.h; define it here
// so we can set it after uv_tcp_open for the SO_REUSEPORT path.
#ifndef UV_HANDLE_BOUND
#define UV_HANDLE_BOUND 0x00002000
#endif

/** @brief Create, bind, and listen on a TCP socket with optional SO_REUSEPORT.
 *
 * Two code paths:
 *
 *   SO_REUSEPORT path (reuseport=true, non-Windows):
 *     Creates a raw socket with socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK),
 *     sets SO_REUSEADDR and SO_REUSEPORT, binds, and listens. The socket
 *     fd is then handed to libuv via uv_tcp_open(). This is used in
 *     multi-worker mode so each worker thread has its own accept loop
 *     sharing the same port. The kernel distributes incoming connections
 *     across the workers in a round-robin fashion.
 *
 *     IMPORTANT: uv_tcp_open() does not set the internal UV_HANDLE_BOUND
 *     flag. Since uv_listen() checks for this flag, we must set it manually
 *     (out_handle->flags |= UV_HANDLE_BOUND) before calling uv_listen().
 *
 *   Standard path (reuseport=false or Windows):
 *     Uses libuv's standard uv_tcp_bind() + uv_listen() sequence. This
 *     works on all platforms but does not support SO_REUSEPORT.
 *
 * @param loop       Event loop to attach the TCP handle to.
 * @param out_handle [out] Initialized TCP handle (managed by libuv, do not
 *                  free by caller).
 * @param port       TCP port number.
 * @param backlog    Maximum length of the pending connections queue.
 * @param reuseport  Enable SO_REUSEPORT for multi-process/thread socket
 * sharing.
 * @return 0 on success, -1 on socket/bind/listen error. */
static int
bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port, int backlog, bool reuseport)
{
#ifndef _WIN32
	if (reuseport) {
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (fd < 0) {
			return -1;
		}
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
		struct sockaddr_in addr;
		uv_ip4_addr("0.0.0.0", port, &addr);
		if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
			close(fd);
			return -1;
		}
		if (listen(fd, backlog) < 0) {
			close(fd);
			return -1;
		}
		int r = uv_tcp_init(loop, out_handle);
		if (r < 0) {
			close(fd);
			return -1;
		}
		r = uv_tcp_open(out_handle, (uv_os_sock_t)fd);
		if (r < 0) {
			close(fd);
			return -1;
		}
		// uv_tcp_open does not set UV_HANDLE_BOUND; uv_listen requires
		// it, so set it manually before calling uv_listen.
		out_handle->flags |= UV_HANDLE_BOUND;
		return uv_listen((uv_stream_t*)out_handle, backlog, on_new_connection);
	}
#endif
	int r = uv_tcp_init(loop, out_handle);
	if (r < 0) {
		return -1;
	}
	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", port, &addr);
	r = uv_tcp_bind(out_handle, (const struct sockaddr*)&addr, 0);
	if (r < 0) {
		return -1;
	}
	return uv_listen((uv_stream_t*)out_handle, backlog, on_new_connection);
}

/** @brief Start the server, bind to the given port, and enter the main event
 * loop (blocking).
 *
 * This is the final step in server startup. The full bootstrap sequence:
 *
 *   1. TLS init: load SSL_CTX with cert + key (if enable_tls is set).
 *      If TLS init fails, the server returns -1 (fail-fast).
 *
 *   2. Async handle: uv_async_init for cross-thread stop signals.
 *      csilk_server_stop() calls uv_async_send() which wakes the event
 *      loop and runs on_stop_async().
 *
 *   3. Bind + listen: bind_and_listen() with SO_REUSEPORT if
 *      worker_threads > 1, otherwise standard single-socket bind.
 *
 *   4. TCP keepalive: if configured, enable TCP keepalive probes to
 *      detect dead peers.
 *
 *   5. Worker threads: if worker_threads > 1, spawn N-1 worker threads
 *      each running their own libuv loop + accept loop (SO_REUSEPORT).
 *      The main thread also runs its own event loop, so total accept
 *      loops = worker_threads.
 *
 *   6. SIGINT handler: register a libuv signal watcher that calls
 *      csilk_server_stop() on SIGINT (Ctrl+C).
 *
 *   7. Fire CSILK_HOOK_SERVER_START.
 *
 *   8. uv_run(): enter the event loop. Blocks until the loop stops
 *      (via csilk_server_stop() or SIGINT).
 *
 * @param server The server instance.
 * @param port   TCP port to bind to.
 * @return The uv_run() return value on exit, or -1 on initialization failure.
 * @note When worker_threads > 1, the main thread runs the event loop and
 *       additional worker threads each run their own independent loop. */
int
csilk_server_run(csilk_server_t* server, int port)
{
	if (!server) {
		return -1;
	}

	if (server->config.enable_tls) {
		init_tls(server);
		if (!server->ssl_ctx) {
			CSILK_LOG_E("Failed to initialize TLS context");
			return -1;
		}
	}

	int workers = server->config.worker_threads;
	if (workers <= 0) {
		workers = 1;
	}

	// Initialize async handle for thread-safe stop
	int r = uv_async_init(server->loop, &server->async_handle, on_stop_async);
	if (r < 0) {
		return -1;
	}
	server->async_handle.data = server;

	r = bind_and_listen(
	    server->loop, &server->server_handle, port, server->config.listen_backlog, workers > 1);
	if (r < 0) {
		return -1;
	}
	server->server_handle.data = server;

	if (server->config.tcp_keepalive > 0) {
		uv_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
	}

	if (workers > 1) {
		int nworkers = workers - 1;
		server->worker_tids = malloc((size_t)nworkers * sizeof(uv_thread_t));
		server->worker_stop_async = calloc((size_t)nworkers, sizeof(uv_async_t));
		if (server->worker_tids && server->worker_stop_async) {
			server->worker_count = nworkers;
			server->worker_stop_count = nworkers;

			pthread_barrier_t barrier;
			pthread_barrier_init(&barrier, NULL, workers);

			for (int i = 0; i < nworkers; i++) {
				worker_data_t* data = malloc(sizeof(worker_data_t));
				if (!data) {
					continue;
				}
				data->server = server;
				data->port = port;
				data->worker_index = i;
				data->barrier = &barrier;
				uv_thread_create(&server->worker_tids[i], worker_thread, data);
			}

			/* Wait for all workers to finish initialising their loops */
			pthread_barrier_wait(&barrier);
			pthread_barrier_destroy(&barrier);
		} else {
			free(server->worker_tids);
			server->worker_tids = NULL;
			free(server->worker_stop_async);
			server->worker_stop_async = NULL;
		}
	}

	r = uv_signal_init(server->loop, &server->sig_handle);
	if (r < 0) {
		uv_close((uv_handle_t*)&server->async_handle, NULL);
		uv_close((uv_handle_t*)&server->server_handle, NULL);
		return -1;
	}
	server->sig_handle.data = server;
	r = uv_signal_start(&server->sig_handle, on_signal, SIGINT);
	if (r < 0) {
		uv_close((uv_handle_t*)&server->sig_handle, NULL);
		uv_close((uv_handle_t*)&server->async_handle, NULL);
		uv_close((uv_handle_t*)&server->server_handle, NULL);
		return -1;
	}

	CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

	trigger_hooks(server, NULL, CSILK_HOOK_SERVER_START);

	return uv_run(server->loop, UV_RUN_DEFAULT);
}

/* --- TLS Helper Implementations --- */

/** @brief Initialize the server's TLS/SSL context using OpenSSL.
 *
 * Loads error strings, initializes SSL algorithms, creates a TLS server
 * method context, loads the certificate chain and private key from the
 * configured file paths, optionally loads a CA file, and optionally enables
 * peer verification. On any failure, the SSL context is freed and set to
 * NULL (TLS is effectively disabled).
 *
 * @param s The server instance (config must have tls_cert_file and
 *          tls_key_file set if enable_tls is true). */
static void
init_tls(csilk_server_t* s)
{
	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();

	const SSL_METHOD* method = TLS_server_method();
	s->ssl_ctx = SSL_CTX_new(method);
	if (!s->ssl_ctx) {
		ERR_print_errors_fp(stderr);
		return;
	}

	if (s->config.tls_cert_file && s->config.tls_key_file) {
		if (SSL_CTX_use_certificate_chain_file(s->ssl_ctx, s->config.tls_cert_file) <= 0) {
			ERR_print_errors_fp(stderr);
			goto error;
		}
		if (SSL_CTX_use_PrivateKey_file(
			s->ssl_ctx, s->config.tls_key_file, SSL_FILETYPE_PEM) <= 0) {
			ERR_print_errors_fp(stderr);
			goto error;
		}
	} else {
		CSILK_LOG_E("TLS enabled but cert/key files missing");
		goto error;
	}

	if (s->config.tls_ca_file) {
		if (SSL_CTX_load_verify_locations(s->ssl_ctx, s->config.tls_ca_file, NULL) <= 0) {
			ERR_print_errors_fp(stderr);
		}
	}

	if (s->config.tls_verify_peer) {
		SSL_CTX_set_verify(
		    s->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	}

	return;

error:
	SSL_CTX_free(s->ssl_ctx);
	s->ssl_ctx = NULL;
}

/** @brief Clean up the server's TLS/SSL context and global SSL state.
 *
 * Frees the SSL_CTX and calls EVP_cleanup() for OpenSSL global cleanup.
 *
 * @param s The server instance (may have ssl_ctx == NULL). */
static void
cleanup_tls(csilk_server_t* s)
{
	if (s->ssl_ctx) {
		SSL_CTX_free(s->ssl_ctx);
		s->ssl_ctx = NULL;
	}
	EVP_cleanup();
}

/** @brief Set up TLS for an individual client connection.
 *
 * Creates a new SSL session from the server's SSL_CTX, initializes memory
 * BIOs for reading and writing encrypted data, and starts the TLS handshake
 * by calling process_tls_read().
 *
 * @param client The client connection to set up TLS on.
 * @return 0 on success, -1 if SSL session creation or BIO setup fails. */
static int
setup_client_tls(csilk_client_t* client)
{
	client->ssl = SSL_new(client->server->ssl_ctx);
	if (!client->ssl) {
		return -1;
	}

	client->read_bio = BIO_new(BIO_s_mem());
	client->write_bio = BIO_new(BIO_s_mem());
	if (!client->read_bio || !client->write_bio) {
		SSL_free(client->ssl);
		client->ssl = NULL;
		return -1;
	}

	SSL_set_bio(client->ssl, client->read_bio, client->write_bio);
	SSL_set_accept_state(client->ssl);

	// Try to start handshake
	process_tls_read(client);
	return 0;
}

/** @brief Process incoming TLS data — complete the handshake or decrypt
 * application data.
 *
 * If the TLS handshake is not yet complete, performs SSL_do_handshake() and
 * flushes the write BIO. If the handshake is complete, calls SSL_read() in
 * a loop to decrypt application data and feeds the decrypted data to the
 * llhttp parser (or WebSocket frame parser).
 *
 * @param client The client connection with pending TLS data in the read BIO. */
static void
process_tls_read(csilk_client_t* client)
{
	char buf[4096];
	int n;

	if (!SSL_is_init_finished(client->ssl)) {
		int r = SSL_do_handshake(client->ssl);
		flush_tls_write(client);
		if (r <= 0) {
			int err = SSL_get_error(client->ssl, r);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
				CSILK_LOG_E("TLS Handshake error: %d", err);
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
			return;
		}
		// Handshake finished, might have data in read BIO
	}

	while ((n = SSL_read(client->ssl, buf, sizeof(buf))) > 0) {
		if (client->ctx.is_websocket) {
			csilk_ws_parse_frame(&client->ctx, (const uint8_t*)buf, (size_t)n);
		} else {
			enum llhttp_errno err = llhttp_execute(&client->parser, buf, (size_t)n);
			if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
				if (err == HPE_CLOSED_CONNECTION) {
					llhttp_init(&client->parser,
						    HTTP_REQUEST,
						    &client->server->settings);
					client->parser.data = client;
				} else {
					fprintf(stderr,
						"TLS Parse error: %s %s\n",
						llhttp_errno_name(err),
						client->parser.reason);
					if (!uv_is_closing((uv_handle_t*)&client->handle)) {
						uv_close((uv_handle_t*)&client->handle, on_close);
					}
					break;
				}
			}
		}
	}

	if (n <= 0) {
		int err = SSL_get_error(client->ssl, n);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE &&
		    err != SSL_ERROR_ZERO_RETURN) {
			CSILK_LOG_E("TLS Read error: %d", err);
			if (!uv_is_closing((uv_handle_t*)&client->handle)) {
				uv_close((uv_handle_t*)&client->handle, on_close);
			}
		}
	}

	flush_tls_write(client);
}

/** @brief Flush buffered TLS encrypted data to the client socket.
 *
 * Reads encrypted data from the write BIO and sends it via libuv write
 * requests. Must be called after SSL_write() or SSL_do_handshake() to
 * ensure the encrypted output is actually transmitted.
 *
 * @param client The client connection whose write BIO should be drained. */
static void
flush_tls_write(csilk_client_t* client)
{
	char buf[4096];
	int n;

	while ((n = BIO_read(client->write_bio, buf, sizeof(buf))) > 0) {
		uv_write_t* req = malloc(sizeof(uv_write_t));
		if (!req) {
			break;
		}

		char* data = malloc((size_t)n);
		if (!data) {
			free(req);
			break;
		}
		memcpy(data, buf, (size_t)n);

		uv_buf_t uv_buf = uv_buf_init(data, (unsigned int)n);
		req->data = data;
		uv_write(req, (uv_stream_t*)&client->handle, &uv_buf, 1, on_write);
	}
}

/** @brief Get the internal message queue instance for the server.
 *
 * The MQ is created automatically during csilk_server_new(). It can be
 * used to register topics, subscribers, and publish messages.
 *
 * @param server The server instance.
 * @return Pointer to the MQ instance, or NULL if server is NULL. */
csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
	return server ? server->mq : NULL;
}

csilk_server_t*
csilk_ctx_get_server(csilk_ctx_t* c)
{
	if (!c) {
		return NULL;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	return client ? client->server : NULL;
}
