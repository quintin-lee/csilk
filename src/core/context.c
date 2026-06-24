/**
 * @file context.c
 * @brief Request/response context implementation.
 *
 * Provides the per-request csilk_ctx_t lifecycle: header hash-map operations
 * (djb2-based, case-insensitive), request/body management, response status
 * and body setup, query parameter parsing, async/chunked response support,
 * and context cleanup.
 *
 * Key design points:
 *   - Headers are stored in a fixed-size hash map (CSILK_HEADER_BUCKETS)
 *     with linked-list chaining. Allocations come from the request arena
 *     for zero-fragmentation cleanup.
 *   - The context carries an arena (bump allocator) for all request-scoped
 *     allocations — path strings, query params, handler chains, header
 *     entries. The entire arena is freed in one shot at request end.
 *   - Query parameters are parsed from the URL query string on demand
 *     and split into key=value pairs.
 *   - Async mode is signalled via ctx->is_async: when true, the response
 *     is NOT sent in on_message_complete; the handler must call
 *     csilk_send() or csilk_stream() explicitly.
 *   - The context also holds driver pointers (storage, crypto, cipher)
 *     inherited from the server at connection time.
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>

#include "core/ctx_internal.h"
#include "core/header_map.h"
#include "core/query.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "core/srv_internal.h"

/** @brief Advance to the next handler in the chain and invoke it.
 *
 * Increments the internal handler index and calls the next handler function.
 * If the request has been aborted (via csilk_abort()), this is a no-op.
 * The handler chain is a nullptr-terminated array; if the next entry is nullptr,
 * execution falls through (the response is sent automatically if not async).
 *
 * @param c The request context.
 * @note Typically called at the end of a middleware or route handler to pass
 *       control to the next handler in the pipeline. */
/** @brief Invoke the next handler in the middleware/route chain.
 *
 * Advances the internal handler index and calls the next non-null handler.
 * If the context has been aborted (via csilk_abort()) or no handlers are
 * registered, this is a no-op.  Each handler is responsible for calling
 * csilk_next() to continue the chain — handlers that set a terminal response
 * (e.g. csilk_string) typically do NOT call csilk_next().
 *
 * @param c The request context. */
void
csilk_next(csilk_ctx_t* c)
{
	if (c->aborted || c->panicked || c->handlers == nullptr) {
		return;
	}
	c->handler_index++;
	if (c->handlers[c->handler_index] != nullptr) {
		c->handlers[c->handler_index](c);
	}
}

/** @brief Abort the handler chain immediately.
 *
 * Sets the aborted flag on the context. Subsequent calls to csilk_next()
 * are ignored. The response is still sent once the current handler returns.
 *
 * @param c The request context.
 * @note This does NOT close the connection — it only prevents further
 *       handlers from executing. */
void
csilk_abort(csilk_ctx_t* c)
{
	c->aborted = 1;
}

/** @brief Get a URL path parameter value by name.
 *
 * Path parameters are extracted from the URL during routing when the route
 * pattern contains :param segments (e.g., "/users/:id").
 *
 * @param c   The request context.
 * @param key Parameter name (as declared in the route pattern without the ':').
 * @return The URL-unescaped parameter value string, or nullptr if not found.
 * @note The returned pointer is heap-allocated and valid until
 *       csilk_ctx_cleanup() is called. */
const char*
csilk_get_param(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}
	for (int i = 0; i < c->params_count; i++) {
		if (strcmp(c->params[i].key, key) == 0) {
			return c->params[i].value;
		}
	}
	return nullptr;
}

/** @brief Get the count of path parameters.
 *
 * @param c The request context.
 * @return Number of parameters. */
int
csilk_get_params_count(csilk_ctx_t* c)
{
	return c ? c->params_count : 0;
}

/** @brief Get the name of a parameter by index.
 *
 * @param c     The request context.
 * @param index Parameter index.
 * @return Parameter name or nullptr. */
const char*
csilk_get_param_key(csilk_ctx_t* c, int index)
{
	if (c && index >= 0 && index < c->params_count) {
		return c->params[index].key;
	}
	return nullptr;
}

/** @brief Get the value of a parameter by index.
 *
 * @param c     The request context.
 * @param index Parameter index.
 * @return Parameter value or nullptr. */
const char*
csilk_get_param_value(csilk_ctx_t* c, int index)
{
	if (c && index >= 0 && index < c->params_count) {
		return c->params[index].value;
	}
	return nullptr;
}

/** @brief Get a request header value by key (case-insensitive).
 *
 * Searches the request header hash map. Key comparison uses strcasecmp so
 * "Content-Type" and "content-type" are treated as equivalent.
 *
 * @param c   The request context.
 * @param key Header key to look up.
 * @return Header value string, or nullptr if the header is not present.
 * @note The returned pointer lives in arena memory (valid until arena reset).
 */
const char*
csilk_get_header(csilk_ctx_t* c, const char* key)
{
	return map_get(&c->request.headers, key);
}

/** @brief Get a response header value by key (case-insensitive).
 *
 * Searches the response header hash map. Header values retrieved here are
 * those that have been set via csilk_set_header() / csilk_add_header().
 *
 * @param c   The request context.
 * @param key Header key to look up.
 * @return Header value string, or nullptr if not found.
 * @note The returned pointer lives in arena memory (valid until arena reset).
 */
const char*
csilk_get_response_header(csilk_ctx_t* c, const char* key)
{
	return map_get(&c->response.headers, key);
}

/** @brief Get a query parameter value by key.
 *
 * Query parameters are populated by csilk_parse_query() which is called
 * automatically during request finalization. The value is URL-decoded.
 *
 * @param c   The request context.
 * @param key Query parameter name.
 * @return The URL-decoded value string, or an empty string if the parameter
 *         was present without a value, or nullptr if the parameter is absent.
 * @note The returned pointer lives in arena memory (valid until arena reset).
 */
const char*
csilk_get_query(csilk_ctx_t* c, const char* key)
{
	return map_get(&c->request.query_params, key);
}

/** @brief Set a request header, overwriting any existing value for the same
 * key.
 *
 * @param c     The request context.
 * @param key   Header key (case-insensitive for matching on subsequent
 * lookups).
 * @param value Header value.
 * @note Both key and value are duplicated into arena memory. */
void
csilk_set_request_header(csilk_ctx_t* c, const char* key, const char* value)
{
	map_set(c, &c->request.headers, key, value);
}

/** @brief Clean up request context resources between requests.
 *
 * Resets the arena allocator for reuse, frees URL path parameters, request
 * body, and path strings. Clears all header maps (request, response, query,
 * form). Cleans up storage items and resets all state flags for the next
 * request. Called after each HTTP request is fully processed.
 *
 * @param c The request context. */
void
csilk_ctx_cleanup(csilk_ctx_t* c)
{
	if (!c) {
		return;
	}

	csilk_ctx_defer_free(c);

	if (c->arena) {
		csilk_arena_reset(c->arena);
	} else {
		for (int i = 0; i < c->params_count; i++) {
			free(c->params[i].key);
			free(c->params[i].value);
		}
	}
	c->params_count = 0;

	/*
	 * request.path is always strdup'd (malloc'd) by
	 * csilk_split_url (or test_utils).  csilk_arena_reset
	 * above does NOT free it — we must free it here.
	 */
	free(c->request.path);
	c->request.path = nullptr;

	if (c->request.body && c->request.body_is_managed) {
		free(c->request.body);
	}
	c->request.body = nullptr;
	c->request.body_len = 0;
	c->request.body_is_managed = 0;

	for (int i = 0; i < c->read_buffers_count; i++) {
		if (c->read_buffers[i]) {
			free(c->read_buffers[i]);
			c->read_buffers[i] = nullptr;
		}
	}
	c->read_buffers_count = 0;

	memset(&c->request.headers, 0, sizeof(csilk_header_map_t));
	memset(&c->request.query_params, 0, sizeof(csilk_header_map_t));
	memset(&c->request.form_params, 0, sizeof(csilk_header_map_t));
	memset(&c->response.headers, 0, sizeof(csilk_header_map_t));

	if (c->response.body && c->response.body_is_managed) {
		free((void*)c->response.body);
		c->response.body = nullptr;
		c->response.body_is_managed = 0;
	}

	if (c->file_fd >= 0) {
		uv_fs_t close_req;
		uv_fs_close(nullptr, &close_req, c->file_fd, nullptr);
		uv_fs_req_cleanup(&close_req);
		c->file_fd = -1;
	}
	c->file_offset = 0;
	c->file_size = 0;

	if (c->storage_driver && c->storage_driver->clear) {
		c->storage_driver->clear(c);
	}
	c->storage_head = nullptr;

	c->aborted = 0;
	c->panicked = 0;
	c->is_websocket = 0;
	c->is_sse = 0;
	c->is_async = 0;
	c->response_started = 0;
	c->handler_index = -1;
	c->current_handler = nullptr;
	c->on_ws_message = nullptr;
	memset(c->request_id, 0, sizeof(c->request_id));
}

/** @brief Get the HTTP method of the current request.
 *
 * Returns the method string as parsed by the HTTP parser (e.g., "GET", "POST",
 * "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS").
 *
 * @param c The request context.
 * @return The method string, or nullptr if the context is nullptr.
 * @note The returned pointer is valid until csilk_ctx_cleanup(). */
const char*
csilk_get_method(csilk_ctx_t* c)
{
	return c ? c->request.method : nullptr;
}

/** @brief Get the URL path of the current request.
 *
 * Returns the decoded URL path (without the query string). For example, a
 * request to "/foo/bar?id=1" yields path "/foo/bar".
 *
 * @param c The request context.
 * @return The URL path string, or nullptr if the context is nullptr.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_path(csilk_ctx_t* c)
{
	return c ? c->request.path : nullptr;
}

/** @brief Get the request body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-nullptr, receives the body length in bytes.
 * @return Pointer to the raw request body, or nullptr if no body or nullptr context.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_body(csilk_ctx_t* c, size_t* out_len)
{
	if (out_len) {
		*out_len = c ? c->request.body_len : 0;
	}
	return c ? c->request.body : nullptr;
}

/** @brief Get the length of the request body.
 *
 * @param c The request context.
 * @return Body length in bytes, or 0 if the context is nullptr or body is empty.
 */
size_t
csilk_get_body_len(csilk_ctx_t* c)
{
	return c ? c->request.body_len : 0;
}

/** @brief Check if the current request is a WebSocket upgrade.
 *
 * @param c The request context.
 * @return 1 if the connection has been upgraded to WebSocket, 0 otherwise.
 * @note Set to 1 by csilk_ws_handshake() after a successful upgrade. */
int
csilk_is_websocket(csilk_ctx_t* c)
{
	return c ? c->is_websocket : 0;
}

/** @brief Enable/disable WebSocket mode.
 *
 * @param c            The request context.
 * @param is_websocket 1 to enable, 0 to disable. */
void
csilk_ctx_set_websocket(csilk_ctx_t* c, int is_websocket)
{
	if (c) {
		c->is_websocket = is_websocket;
	}
}

/** @brief Check if the current connection is a Server-Sent Events stream.
 *
 * @param c The request context.
 * @return 1 if SSE mode is active, 0 otherwise.
 * @note Set externally by the handler that initiates SSE streaming. */
int
csilk_is_sse(csilk_ctx_t* c)
{
	return c ? c->is_sse : 0;
}

/** @brief Enable/disable SSE mode.
 *
 * @param c      The request context.
 * @param is_sse 1 to enable, 0 to disable. */
void
csilk_ctx_set_sse(csilk_ctx_t* c, int is_sse)
{
	if (c) {
		c->is_sse = is_sse;
	}
}

/** @brief Get the internal client connection handle.
 *
 * @param c The request context.
 * @return Opaque pointer to csilk_client_t. */
void*
_csilk_get_internal_client(csilk_ctx_t* c)
{
	return c ? c->_internal_client : nullptr;
}

/** @brief Set the internal client connection handle.
 *
 * @param c      The request context.
 * @param client Opaque pointer to csilk_client_t. */
void
_csilk_set_internal_client(csilk_ctx_t* c, void* client)
{
	if (c) {
		c->_internal_client = client;
	}
}

/** @brief Get the unique request ID string.
 *
 * Returns the UUID v4 request identifier that was generated when the request
 * was first parsed. The ID is formatted as 8-4-4-4-12 hex digits (37 bytes
 * including null terminator).
 *
 * @param c The request context.
 * @return Pointer to the request ID string, or nullptr if context is nullptr.
 * @note The ID is generated via csilk_generate_uuid() and stored inline in
 *       the context. It is valid for the lifetime of the context. */
const char*
csilk_get_request_id(csilk_ctx_t* c)
{
	return c ? c->request_id : nullptr;
}

/** @brief Get the arena allocator associated with the context.
 *
 * The arena is request-scoped and created automatically on each new
 * connection. Use it for short-lived allocations that should live for the
 * duration of the request.
 *
 * @param c The request context.
 * @return Pointer to the arena, or nullptr if context is nullptr.
 * @note All arena memory is reclaimed when the request completes
 *       (via csilk_arena_reset() in csilk_ctx_cleanup()). */
csilk_arena_t*
csilk_get_arena(csilk_ctx_t* c)
{
	return c ? c->arena : nullptr;
}

/** @brief Get the currently set response status code.
 *
 * @param c The request context.
 * @return The HTTP response status code, or 0 if the context is nullptr or
 *         no status has been explicitly set. */
int
csilk_get_status(csilk_ctx_t* c)
{
	return c ? c->response.status : 0;
}

void
csilk_set_status(csilk_ctx_t* c, int status)
{
	if (c) {
		c->response.status = status;
	}
}

csilk_header_map_t*
csilk_get_headers(csilk_ctx_t* c)
{
	return c ? &c->request.headers : nullptr;
}

/** @brief Mark the response as asynchronous (sent later, not automatically).
 *
 * When a response is asynchronous, the framework will NOT automatically
 * send the response after the handler chain completes. The handler is
 * responsible for calling _csilk_send_response() or _csilk_send_data()
 * explicitly after the async operation finishes.
 *
 * @param c       The request context.
 * @param is_async 1 to enable async mode, 0 to disable.
 * @note Async mode is automatically set by csilk_response_write() for
 *       streaming responses. */
void
csilk_ctx_set_async(csilk_ctx_t* c, int is_async)
{
	if (c) {
		c->is_async = is_async;
	}
}

/** @brief Get the server instance owning this context.
 *
 * @param c The request context.
 * @return Pointer to csilk_server_t. */
csilk_server_t*
csilk_ctx_get_server(csilk_ctx_t* c)
{
	return c ? (csilk_server_t*)c->server : nullptr;
}

/** @brief Get the internal MQ instance from the context.
 *
 * @param c The request context.
 * @return Pointer to csilk_mq_t, or nullptr if not available. */
csilk_mq_t*
csilk_ctx_get_mq(csilk_ctx_t* c)
{
	return (c && c->server) ? c->server->mq : nullptr;
}

/** @brief Check if the response is in async mode.
 *
 * @param c The request context.
 * @return 1 if async mode is enabled, 0 otherwise.
 * @note In async mode the framework does not automatically flush the
 *       response — the handler must do it explicitly. */
int
csilk_is_async(csilk_ctx_t* c)
{
	return c ? c->is_async : 0;
}

/** @brief Get the handler chain index.
 *
 * @param c The request context.
 * @return Current handler index. */
int
csilk_get_handler_index(csilk_ctx_t* c)
{
	return c ? c->handler_index : -1;
}

/** @brief Set the request UUID.
 *
 * @param c  The request context.
 * @param id The new UUID string (will be truncated to 36 chars). */
void
csilk_set_request_id(csilk_ctx_t* c, const char* id)
{
	if (c && id) {
		snprintf(c->request_id, sizeof(c->request_id), "%s", id);
	}
}

/** @brief Get the internal libuv work request.
 *
 * @param c The request context.
 * @return Pointer to uv_work_t. */
uv_work_t*
csilk_get_work_req(csilk_ctx_t* c)
{
	return c ? &c->work_req : nullptr;
}

/** @brief Configure zero-copy file transmission.
 *
 * @param c      The request context.
 * @param fd     Open file descriptor.
 * @param offset Byte offset to start sending.
 * @param size   Number of bytes to send. */
void
csilk_set_file_response(csilk_ctx_t* c, int fd, size_t offset, size_t size)
{
	if (c) {
		c->file_fd = fd;
		c->file_offset = offset;
		c->file_size = size;
	}
}

/** @brief Get the zero-copy file descriptor.
 *
 * @param c The request context.
 * @return File descriptor or -1. */
int
csilk_get_file_fd(csilk_ctx_t* c)
{
	return c ? c->file_fd : -1;
}

/** @brief Get the route pattern for the current request.
 *
 * @param c The request context.
 * @return The route path string (e.g., "/users/:id") or nullptr. */
const char*
csilk_ctx_get_handler_path(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->path : nullptr;
}

/** @brief Get the permission required by the current handler.
 *
 * @param c The request context.
 * @return Permission string or nullptr. */
const char*
csilk_ctx_get_handler_perm_required(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->perm_required : nullptr;
}

/** @brief Get the resource pattern for the current handler's permission check.
 *
 * @param c The request context.
 * @return Resource string or nullptr. */
const char*
csilk_ctx_get_handler_perm_resource(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->perm_resource : nullptr;
}

/** @brief Get the response body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-nullptr, receives the response body length.
 * @return Pointer to the response body, or nullptr if no body or nullptr context.
 * @note The body may be managed (arena or heap) depending on how it was set.
 *       The caller must not free the returned pointer. */
const char*
csilk_get_response_body(csilk_ctx_t* c, size_t* out_len)
{
	if (!c) {
		if (out_len) {
			*out_len = 0;
		}
		return nullptr;
	}
	if (out_len) {
		*out_len = c->response.body_len;
	}
	return c->response.body;
}

/** @brief Set the response body directly with explicit ownership semantics.
 *
 * Replaces any existing response body. If the old body was marked as managed
 * it is freed before replacement. The caller specifies whether the new body
 * should be freed automatically during cleanup.
 *
 * @param c       The request context.
 * @param body    Pointer to the body data (may be nullptr).
 * @param len     Body length in bytes.
 * @param managed If non-zero, the framework will free @p body during cleanup.
 * @note Setting managed=1 transfers ownership to the framework. With
 *       managed=0 the caller retains ownership and must keep the pointer
 *       valid until the response is sent. */
void
csilk_set_response_body(csilk_ctx_t* c, const char* body, size_t len, int managed)
{
	if (!c) {
		return;
	}
	if (c->response.body && c->response.body_is_managed) {
		free((void*)c->response.body);
	}
	c->response.body = body;
	c->response.body_len = len;
	c->response.body_is_managed = managed;
}

/** @brief Check if the client has disconnected (connection aborted).
 *
 * @param c The request context.
 * @return 1 if the connection was aborted/closed, 0 otherwise.
 * @note Handlers should check this flag before performing expensive work
 *       for a disconnected client. */
int
csilk_is_aborted(csilk_ctx_t* c)
{
	return c ? c->aborted : 0;
}

/** @brief Internal helper to iterate over all entries in a header map. */
static void
for_each_in_map(csilk_header_map_t* map, csilk_header_cb cb, void* arg)
{
	if (!map || !cb) {
		return;
	}
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = map->buckets[i];
		while (h) {
			if (!cb(h->key, h->value, arg)) {
				return;
			}
			h = h->next;
		}
	}
}

void
csilk_for_each_header(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
	if (c) {
		for_each_in_map(&c->request.headers, cb, arg);
	}
}

void
csilk_for_each_query(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
	if (c) {
		for_each_in_map(&c->request.query_params, cb, arg);
	}
}

void
csilk_for_each_form_field(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
	if (c) {
		for_each_in_map(&c->request.form_params, cb, arg);
	}
}

/** @brief Register a callback for incoming WebSocket messages.
 *
 * The callback is invoked for each data frame received on the WebSocket
 * connection. It receives the context, payload pointer, payload length,
 * and the WebSocket opcode (0x01 for text, 0x02 for binary).
 *
 * @param c  The request context.
 * @param cb Callback function. It receives the context, a pointer to the
 *           unmasked payload data, the payload length, and the frame opcode.
 * @note The payload pointer is valid only within the callback invocation.
 *       If the handler needs the data after the callback returns, it must
 *       copy it. */
void
csilk_set_on_ws_message(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
	if (c) {
		c->on_ws_message = callback;
	}
}

/** @brief Set a callback for outgoing WebSocket frames (testing hook).
 *
 * Registers a callback that is invoked every time the framework sends
 * a WebSocket data frame.  Primarily used in unit tests to intercept
 * and verify the raw frames produced by the server.
 *
 * @param c        The request context.
 * @param callback Callback function receiving the context, payload bytes,
 *                 payload length, and WebSocket opcode.  Pass nullptr to
 *                 clear a previously registered callback.
 * @note The callback runs synchronously on the event-loop thread during
 *       the write path — it must not block or call back into the framework. */
void
csilk_set_on_ws_send(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
	if (c) {
		c->on_ws_send = callback;
	}
}

/** @brief Initialize a request context.
 *
 * Sets up default values for all fields. Should be called for both
 * static (embedded in client) and dynamic (H2 stream) contexts.
 *
 * @param c       The context to initialize.
 * @param s       The owning server instance.
 * @param client  The underlying connection object (csilk_client_t*). */
CSILK_INTERNAL void
_csilk_ctx_init(csilk_ctx_t* c, struct csilk_server_s* s, void* client)
{
	if (!c) {
		return;
	}
	memset(c, 0, sizeof(csilk_ctx_t));
	c->handler_index = -1;
	c->file_fd = -1;
	c->_internal_client = client;
	c->server = s;
	if (s) {
		c->storage_driver = s->storage_driver;
		c->crypto_driver = s->crypto_driver;
		c->cipher_driver = s->cipher_driver;
	}
}

/** @brief Set the storage driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_storage_driver(csilk_ctx_t* c, csilk_storage_driver_t* driver)
{
	if (c) {
		c->storage_driver = driver;
	}
}

/** @brief Set the crypto driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_crypto_driver(csilk_ctx_t* c, csilk_crypto_driver_t* driver)
{
	if (c) {
		c->crypto_driver = driver;
	}
}

/** @brief Set the cipher driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_cipher_driver(csilk_ctx_t* c, csilk_cipher_driver_t* driver)
{
	if (c) {
		c->cipher_driver = driver;
	}
}

/** @brief Get the registered WebSocket callback.
 *
 * @param c The request context.
 * @return Function pointer or nullptr. */
void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t* c,
						const uint8_t* payload,
						size_t len,
						int opcode)
{
	return c ? c->on_ws_message : nullptr;
}
