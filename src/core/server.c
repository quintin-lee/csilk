/**
 * @file server.c
 * @brief Server lifecycle — create, configure, run, stop, free.
 *
 * Implements the server's public API: creation, configuration, driver
 * injection, hook registration, graceful shutdown, and the main event
 * loop (with optional multi-worker SO_REUSEPORT mode).
 *
 * Connection I/O, HTTP parsing, and TLS are delegated to connection.c,
 * http1.c, and tls.c respectively, declared via srv_impl.h.
 * @copyright MIT License
 */

#include <limits.h>
#include <llhttp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "core/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "core/srv_internal.h"
#include "srv_impl.h"

/* --- Signal handler --- */

static void on_server_handle_close(uv_handle_t* handle);

static void
on_signal(uv_signal_t* handle, int signum)
{
	(void)signum;
	csilk_server_t* server = (csilk_server_t*)handle->data;
	csilk_server_stop(server);
}

/* --- Graceful shutdown --- */

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

	_csilk_trigger_hooks(server, nullptr, CSILK_HOOK_SERVER_STOP);

	if (!uv_is_closing((uv_handle_t*)&server->server_handle)) {
		uv_close((uv_handle_t*)&server->server_handle, on_server_handle_close);
	}

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
				if (!uv_is_closing((uv_handle_t*)&client->handle)) {
					uv_close((uv_handle_t*)&client->handle, on_close);
				}
			}
		}
		client = client->next;
	}
	uv_mutex_unlock(&server->clients_mutex);

	if (!uv_is_closing((uv_handle_t*)&server->sig_handle)) {
		uv_close((uv_handle_t*)&server->sig_handle, on_server_handle_close);
	}
	if (!uv_is_closing((uv_handle_t*)&server->async_handle)) {
		uv_close((uv_handle_t*)&server->async_handle, on_server_handle_close);
	}

	for (int i = 1; i < server->worker_pool_count; i++) {
		uv_async_send(&server->worker_pools[i].stop_async);
	}

	if (server->mq) {
		_csilk_mq_free(server->mq);
		server->mq = nullptr;
	}
}

/* --- Server creation --- */

#include "csilk/reflection/reflect.h"

/** @brief Create a new server instance associated with a router.
 *
 * Initializes the reflection system, allocates the server struct, sets up
 * the libuv default loop, configures the llhttp parser callbacks, applies
 * default server configuration (timeouts, buffer limits, backlog), creates
 * a clients mutex and a pool mutex, and creates the internal message queue
 * (MQ) instance.
 *
 * @param router The router instance to use for request matching.
 * @return A new csilk_server_t instance, or nullptr on allocation failure.
 * @note The server must be configured (via csilk_server_set_config()) and
 *       started via csilk_server_run(). Free with csilk_server_free(). */
csilk_server_t*
csilk_server_new(csilk_router_t* router)
{
	csilk_reflect_init();
	csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
	if (!s) {
		return nullptr;
	}
	s->loop = uv_default_loop();
	if (!s->loop) {
		free(s);
		return nullptr;
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

	s->mq = _csilk_mq_new(s->loop);

	return s;
}

/* --- SPA fallback --- */

/** @brief Built-in SPA (Single Page Application) fallback handler.
 *
 * For unmatched GET requests, attempts to serve "index.html" from the
 * configured SPA doc root. This enables client-side routing for SPAs like
 * React, Vue, or Angular that handle their own URL routing in the browser.
 *
 * @param c The request context.
 * @note Only applies to GET requests. Non-GET unmatched requests receive a
 *       standard 404 response. */
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

/* --- Server configuration --- */

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

/** @brief Enable SPA fallback: all unmatched GET requests serve index.html
 * from the given directory.
 *
 * Sets the SPA document root and replaces the 404 handler with the built-in
 * spa_fallback_handler. Overrides any custom 404 handler set via
 * csilk_server_set_not_found_handler().
 *
 * @param server   The server instance.
 * @param doc_root Absolute or relative filesystem path to the directory
 *                 containing index.html.
 * @note The doc_root string is strdup'd internally. Pass nullptr to disable. */
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
 * @return 0 on success, -1 if the limit is reached or parameters are nullptr. */
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

/* --- Server free --- */

/** @brief Free a server instance and all associated resources.
 *
 * Should only be called after the event loop has stopped. Joins any worker
 * threads, frees the SPA doc root, drains the client pool, cleans up TLS,
 * frees the message queue, frees all registered hooks, destroys the clients
 * mutex and pool mutex, and frees the server struct.
 *
 * @param server The server to free (may be nullptr).
 * @note Safe to call with nullptr. After this call the server pointer is
 *       invalid. */
void
csilk_server_free(csilk_server_t* server)
{
	if (!server) {
		return;
	}

	if (server->worker_tids) {
		for (int i = 0; i < server->worker_count; i++) {
			uv_thread_join(&server->worker_tids[i]);
		}
		free(server->worker_tids);
		server->worker_tids = nullptr;
	}

	free(server->spa_doc_root);
	if (server->worker_pools) {
		for (int w = 0; w < server->worker_pool_count; w++) {
			worker_pool_t* wp = &server->worker_pools[w];
			for (int i = 0; i < wp->client_pool_count; i++) {
				free(wp->client_pool[i]);
			}
			for (int i = 0; i < wp->arena_pool_count; i++) {
				csilk_arena_free(wp->arena_pool[i]);
			}
		}
		free(server->worker_pools);
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

	uv_mutex_destroy(&server->clients_mutex);
	free(server);
}

/* --- Server stop --- */

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

/* --- Server stats --- */

/** @brief Read the server's live connection statistics.
 *
 * Provides atomic-safe access to the current active-connection count
 * and the number of pre-allocated client structs in the internal pool.
 * Either output pointer may be nullptr to skip that value.
 *
 * @param server      The server instance (may be nullptr; safe no-op).
 * @param active_conn Out-parameter for the active connection count.
 * @param pooled_conn Out-parameter for the pool size.
 * @note Thread-safe — active_conn is read with an atomic load. */
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
		int total = 0;
		for (int w = 0; w < server->worker_pool_count; w++) {
			total += server->worker_pools[w].client_pool_count;
		}
		*pooled_conn = total;
	}
}

/* --- Server config --- */

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

/* --- Max connections --- */

/** @brief Set the maximum number of concurrent client connections.
 *
 * When this limit is reached, new connections are accepted and immediately
 * closed to drain the listen backlog. A value of 0 means unlimited.
 *
 * @param server The server instance.
 * @param max    Maximum concurrent connections (0 for unlimited).
 * @return The previous maximum connections value, or -1 if server is nullptr. */
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

/* --- Driver injection --- */

/** @brief Set the pluggable storage driver for context key-value operations.
 *
 * When set, calls to csilk_set()/csilk_get() on request contexts belonging
 * to this server will delegate to the driver instead of using the default
 * arena-backed linked list.
 *
 * @param server The server instance.
 * @param driver Pointer to the storage driver vtable (may be nullptr to reset). */
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
 * @param driver Pointer to the crypto driver vtable (may be nullptr to reset). */
void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
	if (server) {
		server->crypto_driver = driver;
	}
}

/** @brief Set the pluggable cipher driver for the server.
 *
 * When set, encryption / decryption operations on request contexts
 * delegate to the supplied driver vtable instead of the built-in
 * software implementation.  Passing nullptr resets to the default.
 *
 * @param server The server instance.
 * @param driver Pointer to the cipher driver vtable (may be nullptr).
 * @note Thread-safe when called before csilk_server_run(); not intended
 *       to be swapped at runtime without external synchronisation. */
void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
	if (server) {
		server->cipher_driver = driver;
	}
}

/* --- Hooks --- */

/** @brief Register a lifecycle hook on the server.
 *
 * Hooks are invoked at specific points in the request lifecycle
 * (conn_open, conn_close, request_begin, request_end, server_start,
 * server_stop). Multiple handlers can be registered for the same hook type;
 * they are called in reverse order of registration (LIFO).
 *
 * @param s       The server instance.
 * @param type    Hook type (CSILK_HOOK_CONN_OPEN through CSILK_HOOK_SERVER_STOP).
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
 * @param c    The request context (may be nullptr for server-level hooks).
 * @param type Hook type to trigger. */
CSILK_INTERNAL void
_csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type)
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

/* --- Worker thread internals --- */

static int
bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port, int backlog, bool reuseport);

/** @brief Per-worker thread initialization data for SO_REUSEPORT multi-loop
 *         mode.
 *
 * Passed to worker_thread() when spawning multiple accept loops. */
typedef struct {
	worker_pool_t* wp; /**< Pre-allocated worker pool (index 1..N-1). */
	int port;
	uv_barrier_t* barrier;
} worker_data_t;

typedef struct {
	uv_loop_t* loop;
	uv_tcp_t* listen_handle;
	csilk_server_t* server;
	int worker_index;
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

	if (!uv_is_closing((uv_handle_t*)sd->listen_handle)) {
		uv_close((uv_handle_t*)sd->listen_handle, nullptr);
	}

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

	if (!uv_is_closing((uv_handle_t*)handle)) {
		uv_close((uv_handle_t*)handle, nullptr);
	}

	/* Drain pending close callbacks (including nested ones from e.g. client
	 * timers), then stop the loop.  The UV_RUN_NOWAIT loop ensures all close
	 * callbacks fire before uv_stop so the loop exits cleanly even under
	 * slow conditions (e.g. GCC coverage builds).  The uv_stop fallback is
	 * needed for io_uring compatibility where uv_run may not exit when all
	 * user handles are closed. */
	for (int i = 0; i < 8; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
	}
	uv_stop(loop);
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
	worker_pool_t* wp = data->wp;
	csilk_server_t* server = wp->server;
	int port = data->port;
	uv_barrier_t* barrier = data->barrier;
	free(data);

	uv_loop_t* loop_ptr = &wp->loop;

#ifdef __APPLE__
	/* Optimize kqueue for OOB data handling on macOS */
	setenv("UV_KQUEUE_OOB", "1", 0);
#endif

	uv_loop_init(loop_ptr);

	wp->server_handle.data = wp;

	_csilk_worker_init_arena_pool(wp);

	if (bind_and_listen(
		loop_ptr, &wp->server_handle, port, server->config.listen_backlog, true) < 0) {
		if (barrier) {
			uv_barrier_wait(barrier);
		}
		uv_loop_close(loop_ptr);
		return;
	}

	worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
	wp->stop_async.data = &sd;
	uv_async_init(loop_ptr, &wp->stop_async, on_worker_stop_async);

	if (barrier) {
		uv_barrier_wait(barrier);
	}

	uv_run(loop_ptr, UV_RUN_DEFAULT);
	uv_loop_close(loop_ptr);
}

#ifndef UV_HANDLE_BOUND
#define UV_HANDLE_BOUND 0x00002000
#endif

/* --- Bind and listen --- */

/** @brief Create, bind, and listen on a TCP socket with optional SO_REUSEPORT.
 *
 * Two code paths:
 *
 *   SO_REUSEPORT path (reuseport=true, non-Windows):
 *     Creates a raw socket with socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK),
 *     sets SO_REUSEADDR and SO_REUSEPORT, binds, and listens. The socket
 *     fd is then handed to libuv via uv_tcp_open(). This is used in
 *     multi-worker mode so each worker thread has its own accept loop
 *     sharing the same port.
 *
 *   Standard path (reuseport=false or Windows):
 *     Uses libuv's standard uv_tcp_bind() + uv_listen() sequence.
 *
 * @param loop       Event loop to attach the TCP handle to.
 * @param out_handle [out] Initialized TCP handle.
 * @param port       TCP port number.
 * @param backlog    Maximum length of the pending connections queue.
 * @param reuseport  Enable SO_REUSEPORT for multi-process/thread socket sharing.
 * @return 0 on success, -1 on socket/bind/listen error. */
static int
bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port, int backlog, bool reuseport)
{
#ifndef _WIN32
	if (reuseport) {
		int fd;
#ifdef __APPLE__
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd >= 0) {
			int flags = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
#else
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
#endif
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

/* --- Server run --- */

/** @brief Start the server, bind to the given port, and enter the main event
 * loop (blocking).
 *
 * The full bootstrap sequence:
 *
 *   1. TLS init: load SSL_CTX with cert + key (if enable_tls is set).
 *   2. Async handle: uv_async_init for cross-thread stop signals.
 *   3. Bind + listen: bind_and_listen() with SO_REUSEPORT if
 *      worker_threads > 1, otherwise standard single-socket bind.
 *   4. TCP keepalive: if configured, enable TCP keepalive probes.
 *   5. Worker threads: if worker_threads > 1, spawn N-1 worker threads
 *      each running their own libuv loop + accept loop (SO_REUSEPORT).
 *   6. SIGINT handler: register a libuv signal watcher.
 *   7. Fire CSILK_HOOK_SERVER_START.
 *   8. uv_run(): enter the event loop.
 *
 * @param server The server instance.
 * @param port   TCP port to bind to.
 * @return The uv_run() return value on exit, or -1 on initialization failure.
 * @note When worker_threads > 1, the main thread runs the event loop and
 *       additional worker threads each run their own independent loop. */
static void
openapi_json_handler(csilk_ctx_t* c)
{
	csilk_server_t* server = csilk_ctx_get_server(c);
	if (server && server->router) {
		csilk_serve_openapi(c,
				    server->router,
				    "Csilk API",
				    "1.0.0",
				    "Auto-generated OpenAPI documentation.");
	} else {
		csilk_set_status(c, 500);
	}
}

int
csilk_server_run(csilk_server_t* server, int port)
{
	if (!server) {
		return -1;
	}

	if (server->config.enable_openapi && server->router) {
		static csilk_handler_t handlers[] = {openapi_json_handler, nullptr};
		csilk_router_add(server->router, "GET", "/openapi.json", handlers, 1);
		CSILK_LOG_I("OpenAPI endpoint automatically registered at GET /openapi.json");
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

	/* Create per-worker pools. Index 0 = main loop (already has loop + server_handle
	 * set up). Worker thread indices 1..workers-1 get their own pools. */
	server->worker_pool_count = workers;
	server->worker_pools = calloc((size_t)workers, sizeof(worker_pool_t));
	if (!server->worker_pools) {
		uv_close((uv_handle_t*)&server->async_handle, nullptr);
		uv_close((uv_handle_t*)&server->server_handle, nullptr);
		return -1;
	}
	server->worker_pools[0].server = server;
	server->worker_pools[0].worker_index = 0;
	server->server_handle.data = &server->worker_pools[0];

	_csilk_worker_init_arena_pool(&server->worker_pools[0]);

	if (server->config.tcp_keepalive > 0) {
		uv_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
	}

	if (workers > 1) {
		int nworkers = workers - 1;
		server->worker_tids = malloc((size_t)nworkers * sizeof(uv_thread_t));
		if (server->worker_tids) {
			server->worker_count = nworkers;

			uv_barrier_t barrier;
			uv_barrier_init(&barrier, (unsigned int)workers);

			for (int i = 0; i < nworkers; i++) {
				int idx = i + 1;
				server->worker_pools[idx].server = server;
				server->worker_pools[idx].worker_index = idx;

				worker_data_t* data = malloc(sizeof(worker_data_t));
				if (!data) {
					continue;
				}
				data->wp = &server->worker_pools[idx];
				data->port = port;
				data->barrier = &barrier;
				uv_thread_create(&server->worker_tids[i], worker_thread, data);
			}

			uv_barrier_wait(&barrier);
			uv_barrier_destroy(&barrier);
		} else {
			free(server->worker_tids);
			server->worker_tids = nullptr;
		}
	}

	r = uv_signal_init(server->loop, &server->sig_handle);
	if (r < 0) {
		uv_close((uv_handle_t*)&server->async_handle, nullptr);
		uv_close((uv_handle_t*)&server->server_handle, nullptr);
		return -1;
	}
	server->sig_handle.data = server;
	r = uv_signal_start(&server->sig_handle, on_signal, SIGINT);
	if (r < 0) {
		uv_close((uv_handle_t*)&server->sig_handle, nullptr);
		uv_close((uv_handle_t*)&server->async_handle, nullptr);
		uv_close((uv_handle_t*)&server->server_handle, nullptr);
		return -1;
	}

	CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

	_csilk_trigger_hooks(server, nullptr, CSILK_HOOK_SERVER_START);

	return uv_run(server->loop, UV_RUN_DEFAULT);
}

/* --- Accessors --- */

/** @brief Get the internal message queue instance for the server.
 *
 * The MQ is created automatically during csilk_server_new(). It can be
 * used to register topics, subscribers, and publish messages.
 *
 * @param server The server instance.
 * @return Pointer to the MQ instance, or nullptr if server is nullptr. */
csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
	return server ? server->mq : nullptr;
}

/** @brief Get the server's radix-tree router.
 *
 * The router is created automatically during csilk_server_new().  It can
 * be used to register routes and middleware before the server is started.
 *
 * @param server The server instance.
 * @return Pointer to the router, or nullptr if server is nullptr. */
csilk_router_t*
csilk_server_get_router(csilk_server_t* server)
{
	return server ? server->router : nullptr;
}

/** @brief Swap the router instance attached to a server.
 *
 * @param server The server instance.
 * @param router The new router instance. */
void
csilk_server_set_router(csilk_server_t* server, csilk_router_t* router)
{
	if (!server || !router) {
		return;
	}
	csilk_router_t* old_router = server->router;
	server->router = router;
	if (old_router) {
		csilk_router_free(old_router);
	}
}
