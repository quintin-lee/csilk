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
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "core/srv_internal.h"

/** @brief Hash a header key string into a bucket index using djb2.
 *
 * Applies the djb2 hash algorithm with case-insensitive character folding
 * (via tolower()) so that "Content-Type" and "content-type" map to the same
 * bucket. This ensures consistent lookups regardless of header casing.
 *
 * @param key Header key string (null-terminated).
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 * @note The caller must ensure @p key is non-nullptr. */
static uint32_t
hash_key(const char* key)
{
	uint32_t hash = 5381;
	int c;
	while ((c = (unsigned char)*key++)) {
		hash = ((hash << 5) + hash) + tolower(c);
	}
	return hash % CSILK_HEADER_BUCKETS;
}

/** @brief Look up a header value by key in the hash map (case-insensitive).
 *
 * Iterates the linked list at the hashed bucket and compares keys using
 * strcasecmp. Returns the first matching value.
 *
 * @param map Header hash map (must not be nullptr).
 * @param key Header key to find (case-insensitive).
 * @return Pointer to the value string, or nullptr if the key is not found.
 * @note The returned string shares the lifetime of the map's arena. */
static const char*
map_get(csilk_header_map_t* map, const char* key)
{
	uint32_t bucket = hash_key(key);
	csilk_header_t* h = map->buckets[bucket];
	while (h) {
		if (strcasecmp(h->key, key) == 0) {
			return h->value;
		}
		h = h->next;
	}
	return nullptr;
}

/** @brief Set a header value in the hash map, overwriting any existing entry.
 *
 * Hashes the key, searches the bucket for an existing entry, and replaces
 * its value. If no entry is found, a new header node is allocated via the
 * context's arena allocator and prepended to the bucket's linked list.
 *
 * @param c     Request context used for arena-based memory allocation.
 * @param map   Header hash map (request or response headers).
 * @param key   Header key (case-insensitive via strcasecmp on lookup).
 * @param value Header value string.
 * @note The key and value are duplicated into arena memory. If the arena is
 *       nullptr this function silently does nothing. */
CSILK_INTERNAL void
map_set(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
	if (!c->arena || !key || !value) {
		return;
	}
	uint32_t bucket = hash_key(key);
	csilk_header_t* h = map->buckets[bucket];
	while (h) {
		if (strcasecmp(h->key, key) == 0) {
			h->value = csilk_arena_strdup(c->arena, value);
			h->value_len = h->value ? strlen(h->value) : 0;
			return;
		}
		h = h->next;
	}

	csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
	if (new_h) {
		new_h->key = csilk_arena_strdup(c->arena, key);
		new_h->key_len = new_h->key ? strlen(new_h->key) : 0;
		new_h->value = csilk_arena_strdup(c->arena, value);
		new_h->value_len = new_h->value ? strlen(new_h->value) : 0;
		new_h->next = map->buckets[bucket];
		map->buckets[bucket] = new_h;
	}
}

/** @brief Add a header value to the hash map, allowing duplicate keys.
 *
 * Unlike map_set(), this function does NOT search for an existing entry with
 * the same key. It always prepends a new header node, so multiple calls with
 * the same key produce multiple entries (e.g., multiple Set-Cookie headers).
 *
 * @param c     Request context for arena-based memory allocation.
 * @param map   Header hash map.
 * @param key   Header key.
 * @param value Header value.
 * @note Both key and value are duplicated into arena memory. Silently does
 *       nothing if the arena is nullptr. */
CSILK_INTERNAL void
map_add(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
	if (!c->arena) {
		return;
	}
	uint32_t bucket = hash_key(key);
	csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
	if (new_h) {
		new_h->key = csilk_arena_strdup(c->arena, key);
		new_h->key_len = strlen(new_h->key);
		new_h->value = csilk_arena_strdup(c->arena, value);
		new_h->value_len = strlen(new_h->value);
		new_h->next = map->buckets[bucket];
		map->buckets[bucket] = new_h;
	}
}

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
	if (c->aborted || c->handlers == nullptr) {
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

	if (c->request.body) {
		free(c->request.body);
		c->request.body = nullptr;
	}

	if (c->request.path) {
		free(c->request.path);
		c->request.path = nullptr;
	}

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
		strncpy(c->request_id, id, 36);
		c->request_id[36] = '\0';
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

/** @brief Store a value in the context's key-value storage.
 *
 * If a storage driver is set on the context, delegates to it. Otherwise,
 * uses a simple linked list allocated from the arena. If the key already
 * exists, its value is replaced. There is a hard limit of 64 storage items
 * per request to prevent excessive arena consumption.
 *
 * @param c     The request context.
 * @param key   Storage key (null-terminated string).
 * @param value Opaque pointer to store (may be nullptr to clear a previous value).
 * @note The key is duplicated into arena memory. The value is stored as a
 *       raw pointer — no deep copy or freeing is performed. */
void
csilk_set(csilk_ctx_t* c, const char* key, void* value)
{
	if (!c || !key) {
		return;
	}

	if (c->storage_driver && c->storage_driver->set) {
		c->storage_driver->set(c, key, value);
		return;
	}

	if (!c->arena) {
		return;
	}

	csilk_storage_item_t* item = c->storage_head;
	int count = 0;
	while (item) {
		if (strcmp(item->key, key) == 0) {
			item->value = value;
			return;
		}
		count++;
		item = item->next;
	}

	/* Limit storage items to prevent excessive allocation in a single request */
	if (count >= CSILK_MAX_STORAGE) {
		CSILK_LOG_E(
		    "Context storage limit reached (%d items) for key: %s", CSILK_MAX_STORAGE, key);
		return;
	}

	csilk_storage_item_t* new_item = csilk_arena_alloc(c->arena, sizeof(csilk_storage_item_t));
	if (new_item) {
		new_item->key = csilk_arena_strdup(c->arena, key);
		new_item->value = value;
		new_item->next = c->storage_head;
		c->storage_head = new_item;
	}
}

/** @brief Retrieve a value from the context's key-value storage.
 *
 * If a storage driver is set on the context, delegates to it. Otherwise,
 * searches the internal linked list for the given key.
 *
 * @param c   The request context.
 * @param key Storage key to look up.
 * @return The value pointer previously stored with csilk_set(), or nullptr if
 *         the key is not found or the context is nullptr. */
void*
csilk_get(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}

	if (c->storage_driver && c->storage_driver->get) {
		return c->storage_driver->get(c, key);
	}

	csilk_storage_item_t* item = c->storage_head;
	while (item) {
		if (strcmp(item->key, key) == 0) {
			return item->value;
		}
		item = item->next;
	}
	return nullptr;
}

int
csilk_set_string(csilk_ctx_t* c, const char* key, const char* value, int ttl_sec)
{
	if (!c || !key || !value) {
		return -1;
	}
	if (c->storage_driver && c->storage_driver->set_string) {
		return c->storage_driver->set_string(c, key, value, ttl_sec);
	}
	/* Fallback: allocate the string in the arena and use standard set.
       Ignores TTL because local storage lives only for the request lifecycle. */
	if (!c->arena) {
		return -1;
	}
	char* arena_val = csilk_arena_strdup(c->arena, value);
	csilk_set(c, key, arena_val);
	return 0;
}

char*
csilk_get_string(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}
	if (c->storage_driver && c->storage_driver->get_string) {
		return c->storage_driver->get_string(c, key);
	}
	/* Fallback: return a strdup of the arena-stored string so caller can free it */
	void* val = csilk_get(c, key);
	if (val) {
		return strdup((const char*)val);
	}
	return nullptr;
}

long long
csilk_incr(csilk_ctx_t* c, const char* key, int ttl_sec)
{
	if (!c || !key) {
		return -1;
	}
	if (c->storage_driver && c->storage_driver->incr) {
		return c->storage_driver->incr(c, key, ttl_sec);
	}
	return -1; /* Local storage doesn't support persistent counters */
}

/** @brief Parse the request body as JSON using cJSON.
 *
 * @param c The request context.
 * @return A cJSON object parsed from the request body, or nullptr if the body
 *         is nullptr or the JSON is invalid.
 * @note The caller owns the returned cJSON object and must free it with
 *       cJSON_Delete(). For error details use csilk_bind_json_err(). */
cJSON*
csilk_bind_json(csilk_ctx_t* c)
{
	if (!c || !c->request.body) {
		return nullptr;
	}
	return cJSON_Parse(c->request.body);
}

/** @brief Parse the request body as JSON with detailed error feedback.
 *
 * Like csilk_bind_json() but sets @p error to a descriptive string on
 * failure (e.g., "Null context", "No request body", or the cJSON parse
 * error position).
 *
 * @param c     The request context.
 * @param error [out] Optional pointer to receive a static error string.
 * @return A cJSON object parsed from the request body, or nullptr on failure.
 * @note The caller owns the returned cJSON object and must free it.
 *       The @p error string is a static pointer (do not free). */
cJSON*
csilk_bind_json_err(csilk_ctx_t* c, const char** error)
{
	if (error) {
		*error = nullptr;
	}
	if (!c) {
		if (error) {
			*error = "Null context";
		}
		return nullptr;
	}
	if (!c->request.body) {
		if (error) {
			*error = "No request body";
		}
		return nullptr;
	}
	cJSON* json = cJSON_Parse(c->request.body);
	if (!json) {
		if (error) {
			*error = cJSON_GetErrorPtr();
		}
		if (error && !*error) {
			*error = "Invalid JSON";
		}
		return nullptr;
	}
	return json;
}

/** @brief Get a cookie value by its name from the Cookie header.
 *
 * Parses the "Cookie" request header by splitting on "; " and then on "=".
 * Returns the value for the first cookie matching @p name.
 *
 * @param c    The request context.
 * @param name Cookie name to look up.
 * @return The cookie value string (arena-allocated), or nullptr if the cookie
 *         is not found, the header is absent, or the context/arena is nullptr.
 * @note The returned value is URL-decoded only as much as the raw header
 *       contains. Cookie attributes (path, domain, etc.) are not supported. */
const char*
csilk_get_cookie(csilk_ctx_t* c, const char* name)
{
	if (!c || !name || !c->arena) {
		return nullptr;
	}
	const char* cookie_header = csilk_get_header(c, "Cookie");
	if (!cookie_header) {
		return nullptr;
	}

	char* cookies = csilk_arena_strdup(c->arena, cookie_header);
	if (!cookies) {
		return nullptr;
	}

	char* saveptr;
	char* cookie = strtok_r(cookies, "; ", &saveptr);

	while (cookie) {
		char* eq = strchr(cookie, '=');
		if (eq) {
			*eq = '\0';
			if (strcmp(cookie, name) == 0) {
				return csilk_arena_strdup(c->arena, eq + 1);
			}
		}
		cookie = strtok_r(nullptr, "; ", &saveptr);
	}

	return nullptr;
}

/** @brief Bind the request body JSON to a registered struct via reflection.
 *
 * Deserializes the JSON request body into the provided struct pointer using
 * the csilk reflection engine. If @p type_name is nullptr, the type is inferred
 * from the current handler's input_type metadata (if available).
 *
 * @param c         The request context.
 * @param type_name Registered type name (e.g., "my_request_t"), or nullptr to
 *                  infer from the route handler's metadata.
 * @param ptr       Pointer to the target struct to populate.
 * @return 1 on success, 0 on failure (nullptr context, no body, type not found,
 *         or JSON parse error).
 * @note Uses csilk_json_unmarshal() internally. The struct should have been
 *       registered via CSILK_REGISTER_REFLECT(). */
int
csilk_bind_reflect(csilk_ctx_t* c, const char* type_name, void* ptr)
{
	if (!c || !c->request.body || !ptr) {
		return 0;
	}
	if (!type_name && c->current_handler) {
		type_name = c->current_handler->input_type;
	}
	if (!type_name) {
		return 0;
	}
	return csilk_json_unmarshal(type_name, c->request.body, ptr);
}

/** @brief Parse a raw query string into the context's query_params hash map.
 *
 * Splits the query string on '&' and each key-value pair on '=', URL-decodes
 * both keys and values, and adds them to the request's query_params map.
 * Parameters without a '=' get an empty-string value.
 *
 * @param c            The request context.
 * @param query_string The raw query string (e.g., "foo=1&bar=baz"). The
 *                     leading '?' should NOT be included.
 * @note The query string is duplicated into arena memory before parsing,
 *       so the original string can be freed immediately after this call.
 *       This is called automatically during request finalization. */
void
csilk_parse_query(csilk_ctx_t* c, const char* query_string)
{
	if (!query_string || *query_string == '\0' || !c->arena) {
		return;
	}

	char* qs = csilk_arena_strdup(c->arena, query_string);
	if (!qs) {
		return;
	}

	char* pos = qs;
	while (pos && *pos) {
		char* amp = strchr(pos, '&');
		if (amp) {
			*amp = '\0';
		}

		char* eq = strchr(pos, '=');
		char* key = pos;
		char* value = nullptr;

		if (eq) {
			*eq = '\0';
			value = eq + 1;
		} else {
			value = "";
		}

		if (*key != '\0') {
			csilk_url_decode(key);
			if (value && *value != '\0') {
				csilk_url_decode(value);
			}
			map_add(c, &c->request.query_params, key, value);
		}

		if (amp) {
			pos = amp + 1;
		} else {
			pos = nullptr;
		}
	}
}

/** @brief Parse the request body as application/x-www-form-urlencoded.
 *
 * Checks the Content-Type header for "application/x-www-form-urlencoded"
 * and, if matched, parses the request body into the form_params hash map.
 * Key-value parsing and URL-decoding follow the same logic as
 * csilk_parse_query().
 *
 * @param c The request context.
 * @note This function must be called explicitly by the handler; it is NOT
 *       invoked automatically. Typically called at the start of a handler
 *       that expects form data. */
void
csilk_parse_form_urlencoded(csilk_ctx_t* c)
{
	if (!c || !c->arena) {
		return;
	}
	const char* body = csilk_get_body(c, nullptr);
	if (!body || *body == '\0') {
		return;
	}

	const char* ct = csilk_get_header(c, "Content-Type");
	if (!ct) {
		return;
	}
	if (strncmp(ct, "application/x-www-form-urlencoded", 33) != 0) {
		return;
	}

	char* qs = csilk_arena_strdup(c->arena, body);
	if (!qs) {
		return;
	}

	char* pos = qs;
	while (pos && *pos) {
		char* amp = strchr(pos, '&');
		if (amp) {
			*amp = '\0';
		}

		char* eq = strchr(pos, '=');
		char* key = pos;
		char* value = nullptr;

		if (eq) {
			*eq = '\0';
			value = eq + 1;
		} else {
			value = "";
		}

		if (*key != '\0') {
			csilk_url_decode(key);
			if (value && *value != '\0') {
				csilk_url_decode(value);
			}
			map_add(c, &c->request.form_params, key, value);
		}

		if (amp) {
			pos = amp + 1;
		} else {
			pos = nullptr;
		}
	}
}

/** @brief Get a form urlencoded field value by key.
 *
 * Looks up the given key in the request's form_params hash map, populated
 * by a prior call to csilk_parse_form_urlencoded().
 *
 * @param c   The request context.
 * @param key Field name to look up.
 * @return The URL-decoded field value, or nullptr if not found.
 * @note The returned pointer lives in arena memory. */
const char*
csilk_get_form_field(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}
	return map_get(&c->request.form_params, key);
}

/* --- Deferred Cleanup (panic-safe resource management) ---
 *
 * Deferred cleanups are registered during normal handler execution and
 * automatically invoked when csilk_panic() triggers a longjmp back to
 * the recovery handler.  Because longjmp does NOT unwind the C stack
 * (destructors and free() calls in downstream stack frames are skipped),
 * deferred cleanups are the only way to safely release resources across
 * a panic boundary.  Items are arena-allocated so they do not need to be
 * individually freed — the arena is cleaned up when the request context
 * is destroyed.
 */

/** @brief Register a cleanup function to run on panic.
 *
 * The function will be executed by csilk_ctx_defer_free() when recovery
 * catches a panic.  Allocated in arena memory so it is cleaned up with
 * the request context after the response is sent.
 *
 * @param c   The request context.
 * @param fn  Cleanup function (e.g. close an fd, release a mutex).
 * @param arg User data passed to @p fn.
 * @return 0 if registered, -1 if context/arena/fn is invalid. */
int
csilk_ctx_defer(csilk_ctx_t* c, void (*fn)(void*), void* arg)
{
	if (!c || !fn || !c->arena) {
		return -1;
	}

	csilk_defer_item_t* item = csilk_arena_alloc(c->arena, sizeof(csilk_defer_item_t));
	if (!item) {
		return -1;
	}

	item->fn = fn;
	item->arg = arg;
	item->next = c->defer_head;
	c->defer_head = item;

	return 0;
}

/** @brief Execute all registered deferred cleanups in reverse order.
 *
 * Called by the recovery handler after catching a panic.  Walks the
 * deferred item linked list (LIFO — most recently registered first) and
 * invokes each cleanup function.  The list is cleared by this call.
 *
 * @param c The request context. */
void
csilk_ctx_defer_free(csilk_ctx_t* c)
{
	if (!c) {
		return;
	}

	csilk_defer_item_t* item = c->defer_head;
	while (item) {
		if (item->fn) {
			item->fn(item->arg);
		}
		item = item->next;
	}
	c->defer_head = nullptr;
}
