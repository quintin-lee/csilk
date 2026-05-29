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

#include "context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "server_internal.h"

/** @brief Hash a header key string into a bucket index using djb2.
 *
 * Applies the djb2 hash algorithm with case-insensitive character folding
 * (via tolower()) so that "Content-Type" and "content-type" map to the same
 * bucket. This ensures consistent lookups regardless of header casing.
 *
 * @param key Header key string (null-terminated).
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 * @note The caller must ensure @p key is non-NULL. */
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
 * @param map Header hash map (must not be NULL).
 * @param key Header key to find (case-insensitive).
 * @return Pointer to the value string, or NULL if the key is not found.
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
	return NULL;
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
 *       NULL this function silently does nothing. */
static void
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
 *       nothing if the arena is NULL. */
static void
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
 * The handler chain is a NULL-terminated array; if the next entry is NULL,
 * execution falls through (the response is sent automatically if not async).
 *
 * @param c The request context.
 * @note Typically called at the end of a middleware or route handler to pass
 *       control to the next handler in the pipeline. */
void
csilk_next(csilk_ctx_t* c)
{
	if (c->aborted || c->handlers == NULL) {
		return;
	}
	c->handler_index++;
	if (c->handlers[c->handler_index] != NULL) {
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

/** @brief Set the HTTP response status code.
 *
 * @param c      The request context.
 * @param status HTTP status code (e.g., 200, 404, 500).
 * @note Also accessible via CSILK_STATUS_OK, CSILK_STATUS_NOT_FOUND, etc. */
void
csilk_status(csilk_ctx_t* c, int status)
{
	c->response.status = status;
}

/** @brief Set the response body as plain text with a status code.
 *
 * If the context has an arena allocator, the body string is duplicated into
 * arena memory. Otherwise, it falls back to strdup() and marks the body as
 * managed (so it will be freed during cleanup). The Content-Type header is
 * NOT set automatically — callers should set it explicitly if needed.
 *
 * @param c      The request context.
 * @param status HTTP status code for the response.
 * @param msg    Plain text body (may be NULL).
 * @note Ownership: when arena is unavailable, the strdup'd copy is freed
 *       automatically during csilk_ctx_cleanup(). Safe to pass NULL for msg. */
void
csilk_string(csilk_ctx_t* c, int status, const char* msg)
{
	c->response.status = status;
	size_t msg_len = msg ? strlen(msg) : 0;
	if (c->arena) {
		c->response.body = msg ? csilk_arena_strdup(c->arena, msg) : NULL;
		c->response.body_len = msg_len;
		c->response.body_is_managed = 0;
	} else {
		if (c->response.body && c->response.body_is_managed) {
			free((void*)c->response.body);
		}
		char* body = msg ? strdup(msg) : NULL;
		c->response.body = body;
		c->response.body_len = body ? msg_len : 0;
		c->response.body_is_managed = body ? 1 : 0;
	}
}

/** @brief Get a URL path parameter value by name.
 *
 * Path parameters are extracted from the URL during routing when the route
 * pattern contains :param segments (e.g., "/users/:id").
 *
 * @param c   The request context.
 * @param key Parameter name (as declared in the route pattern without the ':').
 * @return The URL-unescaped parameter value string, or NULL if not found.
 * @note The returned pointer is heap-allocated and valid until
 *       csilk_ctx_cleanup() is called. */
const char*
csilk_get_param(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return NULL;
	}
	for (int i = 0; i < c->params_count; i++) {
		if (strcmp(c->params[i].key, key) == 0) {
			return c->params[i].value;
		}
	}
	return NULL;
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
 * @return Parameter name or NULL. */
const char*
csilk_get_param_key(csilk_ctx_t* c, int index)
{
	if (c && index >= 0 && index < c->params_count) {
		return c->params[index].key;
	}
	return NULL;
}

/** @brief Get the value of a parameter by index.
 *
 * @param c     The request context.
 * @param index Parameter index.
 * @return Parameter value or NULL. */
const char*
csilk_get_param_value(csilk_ctx_t* c, int index)
{
	if (c && index >= 0 && index < c->params_count) {
		return c->params[index].value;
	}
	return NULL;
}

/** @brief Get a request header value by key (case-insensitive).
 *
 * Searches the request header hash map. Key comparison uses strcasecmp so
 * "Content-Type" and "content-type" are treated as equivalent.
 *
 * @param c   The request context.
 * @param key Header key to look up.
 * @return Header value string, or NULL if the header is not present.
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
 * @return Header value string, or NULL if not found.
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
 *         was present without a value, or NULL if the parameter is absent.
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

/** @brief Set a response header, overwriting any existing value for the same
 * key.
 *
 * @param c     The request context.
 * @param key   Header key.
 * @param value Header value.
 * @note Both key and value are duplicated into arena memory.
 * @note To allow duplicate values (e.g., multiple Set-Cookie), use
 *       csilk_add_header() instead. */
void
csilk_set_header(csilk_ctx_t* c, const char* key, const char* value)
{
	map_set(c, &c->response.headers, key, value);
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
		c->request.body = NULL;
	}

	if (c->request.path) {
		free(c->request.path);
		c->request.path = NULL;
	}

	memset(&c->request.headers, 0, sizeof(csilk_header_map_t));
	memset(&c->request.query_params, 0, sizeof(csilk_header_map_t));
	memset(&c->request.form_params, 0, sizeof(csilk_header_map_t));
	memset(&c->response.headers, 0, sizeof(csilk_header_map_t));

	if (c->response.body && c->response.body_is_managed) {
		free((void*)c->response.body);
		c->response.body = NULL;
		c->response.body_is_managed = 0;
	}

	if (c->file_fd >= 0) {
		uv_fs_t close_req;
		uv_fs_close(NULL, &close_req, c->file_fd, NULL);
		uv_fs_req_cleanup(&close_req);
		c->file_fd = -1;
	}
	c->file_offset = 0;
	c->file_size = 0;

	if (c->storage_driver && c->storage_driver->clear) {
		c->storage_driver->clear(c);
	}
	c->storage_head = NULL;

	c->aborted = 0;
	c->is_websocket = 0;
	c->is_sse = 0;
	c->is_async = 0;
	c->response_started = 0;
	c->handler_index = -1;
	c->current_handler = NULL;
	c->on_ws_message = NULL;
	memset(c->request_id, 0, sizeof(c->request_id));
}

/** @brief Get the HTTP method of the current request.
 *
 * Returns the method string as parsed by the HTTP parser (e.g., "GET", "POST",
 * "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS").
 *
 * @param c The request context.
 * @return The method string, or NULL if the context is NULL.
 * @note The returned pointer is valid until csilk_ctx_cleanup(). */
const char*
csilk_get_method(csilk_ctx_t* c)
{
	return c ? c->request.method : NULL;
}

/** @brief Get the URL path of the current request.
 *
 * Returns the decoded URL path (without the query string). For example, a
 * request to "/foo/bar?id=1" yields path "/foo/bar".
 *
 * @param c The request context.
 * @return The URL path string, or NULL if the context is NULL.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_path(csilk_ctx_t* c)
{
	return c ? c->request.path : NULL;
}

/** @brief Get the request body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-NULL, receives the body length in bytes.
 * @return Pointer to the raw request body, or NULL if no body or NULL context.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_body(csilk_ctx_t* c, size_t* out_len)
{
	if (out_len) {
		*out_len = c ? c->request.body_len : 0;
	}
	return c ? c->request.body : NULL;
}

/** @brief Get the length of the request body.
 *
 * @param c The request context.
 * @return Body length in bytes, or 0 if the context is NULL or body is empty.
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
csilk_set_websocket(csilk_ctx_t* c, int is_websocket)
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
csilk_set_sse(csilk_ctx_t* c, int is_sse)
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
	return c ? c->_internal_client : NULL;
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
 * @return Pointer to the request ID string, or NULL if context is NULL.
 * @note The ID is generated via csilk_generate_uuid() and stored inline in
 *       the context. It is valid for the lifetime of the context. */
const char*
csilk_get_request_id(csilk_ctx_t* c)
{
	return c ? c->request_id : NULL;
}

/** @brief Get the arena allocator associated with the context.
 *
 * The arena is request-scoped and created automatically on each new
 * connection. Use it for short-lived allocations that should live for the
 * duration of the request.
 *
 * @param c The request context.
 * @return Pointer to the arena, or NULL if context is NULL.
 * @note All arena memory is reclaimed when the request completes
 *       (via csilk_arena_reset() in csilk_ctx_cleanup()). */
csilk_arena_t*
csilk_get_arena(csilk_ctx_t* c)
{
	return c ? c->arena : NULL;
}

/** @brief Get the currently set response status code.
 *
 * @param c The request context.
 * @return The HTTP response status code, or 0 if the context is NULL or
 *         no status has been explicitly set. */
int
csilk_get_status(csilk_ctx_t* c)
{
	return c ? c->response.status : 0;
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
csilk_set_async(csilk_ctx_t* c, int is_async)
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
	return c ? (csilk_server_t*)c->server : NULL;
}

/** @brief Get the internal MQ instance from the context.
 *
 * @param c The request context.
 * @return Pointer to csilk_mq_t, or NULL if not available. */
csilk_mq_t*
csilk_ctx_get_mq(csilk_ctx_t* c)
{
	return (c && c->server) ? c->server->mq : NULL;
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
	return c ? &c->work_req : NULL;
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
 * @return The route path string (e.g., "/users/:id") or NULL. */
const char*
csilk_ctx_get_handler_path(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->path : NULL;
}

/** @brief Get the permission required by the current handler.
 *
 * @param c The request context.
 * @return Permission string or NULL. */
const char*
csilk_ctx_get_handler_perm_required(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->perm_required : NULL;
}

/** @brief Get the resource pattern for the current handler's permission check.
 *
 * @param c The request context.
 * @return Resource string or NULL. */
const char*
csilk_ctx_get_handler_perm_resource(csilk_ctx_t* c)
{
	return (c && c->current_handler) ? c->current_handler->perm_resource : NULL;
}

/** @brief Get the response body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-NULL, receives the response body length.
 * @return Pointer to the response body, or NULL if no body or NULL context.
 * @note The body may be managed (arena or heap) depending on how it was set.
 *       The caller must not free the returned pointer. */
const char*
csilk_get_response_body(csilk_ctx_t* c, size_t* out_len)
{
	if (!c) {
		if (out_len) {
			*out_len = 0;
		}
		return NULL;
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
 * @param body    Pointer to the body data (may be NULL).
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
csilk_set_on_ws_message(csilk_ctx_t* c,
			void (*cb)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
	if (c) {
		c->on_ws_message = cb;
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
 * @return Function pointer or NULL. */
void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t* c,
						const uint8_t* payload,
						size_t len,
						int opcode)
{
	return c ? c->on_ws_message : NULL;
}

/** @brief Redirect the client to a different URL with a specific status code.
 *
 * Sets the Location header, updates the response status, and aborts the
 * handler chain. If the provided status is outside the 3xx range, it is
 * coerced to 302 (Found).
 *
 * @param c        The request context.
 * @param status   HTTP redirect status (typically 301, 302, 303, 307, 308).
 * @param location The target URL for the Location header.
 * @note After calling this function the handler chain is aborted and no
 *       further handlers execute. */
void
csilk_redirect(csilk_ctx_t* c, int status, const char* location)
{
	if (!c || !location) {
		return;
	}
	if (status < 300 || status > 308) {
		status = CSILK_STATUS_FOUND;
	}
	csilk_set_header(c, "Location", location);
	c->response.status = status;
	csilk_abort(c);
}

/** @brief Redirect to another URL using the default status code 302 (Found).
 *
 * Convenience wrapper around csilk_redirect().
 *
 * @param c   The request context.
 * @param url The target URL for the redirect. */
void
csilk_redirect_simple(csilk_ctx_t* c, const char* url)
{
	csilk_redirect(c, CSILK_STATUS_FOUND, url);
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
 * @param value Opaque pointer to store (may be NULL to clear a previous value).
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
 * @return The value pointer previously stored with csilk_set(), or NULL if
 *         the key is not found or the context is NULL. */
void*
csilk_get(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return NULL;
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
	return NULL;
}

/** @brief Parse the request body as JSON using cJSON.
 *
 * @param c The request context.
 * @return A cJSON object parsed from the request body, or NULL if the body
 *         is NULL or the JSON is invalid.
 * @note The caller owns the returned cJSON object and must free it with
 *       cJSON_Delete(). For error details use csilk_bind_json_err(). */
cJSON*
csilk_bind_json(csilk_ctx_t* c)
{
	if (!c || !c->request.body) {
		return NULL;
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
 * @return A cJSON object parsed from the request body, or NULL on failure.
 * @note The caller owns the returned cJSON object and must free it.
 *       The @p error string is a static pointer (do not free). */
cJSON*
csilk_bind_json_err(csilk_ctx_t* c, const char** error)
{
	if (error) {
		*error = NULL;
	}
	if (!c) {
		if (error) {
			*error = "Null context";
		}
		return NULL;
	}
	if (!c->request.body) {
		if (error) {
			*error = "No request body";
		}
		return NULL;
	}
	cJSON* json = cJSON_Parse(c->request.body);
	if (!json) {
		if (error) {
			*error = cJSON_GetErrorPtr();
		}
		if (error && !*error) {
			*error = "Invalid JSON";
		}
		return NULL;
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
 * @return The cookie value string (arena-allocated), or NULL if the cookie
 *         is not found, the header is absent, or the context/arena is NULL.
 * @note The returned value is URL-decoded only as much as the raw header
 *       contains. Cookie attributes (path, domain, etc.) are not supported. */
const char*
csilk_get_cookie(csilk_ctx_t* c, const char* name)
{
	if (!c || !name || !c->arena) {
		return NULL;
	}
	const char* cookie_header = csilk_get_header(c, "Cookie");
	if (!cookie_header) {
		return NULL;
	}

	char* cookies = csilk_arena_strdup(c->arena, cookie_header);
	if (!cookies) {
		return NULL;
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
		cookie = strtok_r(NULL, "; ", &saveptr);
	}

	return NULL;
}

/** @brief Add a response header, allowing multiple values for the same key.
 *
 * Unlike csilk_set_header(), this function prepends a new header node
 * without removing any existing values for the same key. This is useful
 * for headers like Set-Cookie where multiple values are allowed.
 *
 * @param c     The request context.
 * @param key   Header key.
 * @param value Header value.
 * @note Both key and value are duplicated into arena memory. */
void
csilk_add_header(csilk_ctx_t* c, const char* key, const char* value)
{
	map_add(c, &c->response.headers, key, value);
}

/** @brief Set a cookie in the response via Set-Cookie header.
 *
 * Constructs a properly formatted Set-Cookie header with the given name,
 * value, and attributes. The cookie is added using csilk_add_header() so
 * multiple cookies can be set on the same response.
 *
 * @param c        The request context.
 * @param name     Cookie name (cannot be NULL).
 * @param value    Cookie value (cannot be NULL).
 * @param max_age  Cookie Max-Age in seconds. Pass 0 to omit, negative for
 *                 immediate expiry (Max-Age=0), positive for a future expiry.
 * @param path     Cookie path (pass NULL for default "/").
 * @param domain   Cookie domain (pass NULL to omit).
 * @param secure   If non-zero, adds the Secure flag.
 * @param http_only If non-zero, adds the HttpOnly flag.
 * @note The cookie is arena-allocated. The name+value and attribute strings
 *       should not contain characters that break cookie formatting. */
void
csilk_set_cookie(csilk_ctx_t* c,
		 const char* name,
		 const char* value,
		 int max_age,
		 const char* path,
		 const char* domain,
		 int secure,
		 int http_only)
{
	if (!c->arena) {
		return;
	}
	size_t buf_size = strlen(name) + strlen(value) + 256; // 256 for attributes
	if (path) {
		buf_size += strlen(path);
	}
	if (domain) {
		buf_size += strlen(domain);
	}

	char* buf = csilk_arena_alloc(c->arena, buf_size);
	if (!buf) {
		return;
	}

	int pos = snprintf(buf, buf_size, "%s=%s", name, value);

	if (max_age > 0) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Max-Age=%d", max_age);
	} else if (max_age < 0) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Max-Age=0");
	}

	if (path) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Path=%s", path);
	} else {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Path=/");
	}

	if (domain) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Domain=%s", domain);
	}

	if (secure) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Secure");
	}

	if (http_only) {
		pos += snprintf(buf + pos, buf_size - (size_t)pos, "; HttpOnly");
	}

	csilk_add_header(c, "Set-Cookie", buf);
}

/** @brief Send a JSON response. The cJSON object is freed by this call.
 *
 * Sets the Content-Type header to "application/json", serializes the cJSON
 * object to an unformatted JSON string, and sets it as the response body.
 * The cJSON object is deleted (freed) after serialization — the caller must
 * NOT free it.
 *
 * @param c      The request context.
 * @param status HTTP status code for the response.
 * @param json   cJSON object to serialize. Ownership is taken by this call.
 * @note If there is a previous body marked as managed, it is freed first.
 *       The serialized JSON string is heap-allocated and managed by the
 *       framework. */
void
csilk_json(csilk_ctx_t* c, int status, cJSON* json)
{
	if (!c || !json) {
		return;
	}

	c->response.status = status;
	csilk_set_header(c, "Content-Type", "application/json");

	if (c->response.body && c->response.body_is_managed) {
		free((void*)c->response.body);
		c->response.body = NULL;
		c->response.body_is_managed = 0;
	}

	char* body = cJSON_PrintUnformatted(json);
	if (body) {
		c->response.body = body;
		c->response.body_len = strlen(body);
		c->response.body_is_managed = 1;
	}
	cJSON_Delete(json);
}

/** @brief Send a JSON error response containing an "error" field.
 *
 * Creates a JSON object with a single "error" key containing @p message
 * and sends it as the response via csilk_json().
 *
 * @param c       The request context.
 * @param status  HTTP status code.
 * @param message Error message string (if NULL, "Unknown error" is used). */
void
csilk_json_error(csilk_ctx_t* c, int status, const char* message)
{
	if (!c) {
		return;
	}
	cJSON* err = cJSON_CreateObject();
	if (!err) {
		return;
	}
	cJSON_AddStringToObject(err, "error", message ? message : "Unknown error");
	csilk_json(c, status, err);
}

/** @brief Bind the request body JSON to a registered struct via reflection.
 *
 * Deserializes the JSON request body into the provided struct pointer using
 * the csilk reflection engine. If @p type_name is NULL, the type is inferred
 * from the current handler's input_type metadata (if available).
 *
 * @param c         The request context.
 * @param type_name Registered type name (e.g., "my_request_t"), or NULL to
 *                  infer from the route handler's metadata.
 * @param ptr       Pointer to the target struct to populate.
 * @return 1 on success, 0 on failure (NULL context, no body, type not found,
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

/** @brief Send a JSON response from a registered struct via reflection.
 *
 * Serializes the provided struct to JSON using the csilk reflection engine
 * and sends it as the HTTP response. If @p type_name is NULL, the type is
 * inferred from the current handler's output_type metadata.
 *
 * @param c         The request context.
 * @param status    HTTP status code.
 * @param type_name Registered type name, or NULL to infer from route metadata.
 * @param ptr       Pointer to the struct to serialize.
 * @note The serialized JSON string is heap-allocated and managed by the
 *       framework (freed during cleanup). Uses csilk_json_marshal() internally.
 */
void
csilk_json_reflect(csilk_ctx_t* c, int status, const char* type_name, const void* ptr)
{
	if (!c || !ptr) {
		return;
	}
	if (!type_name && c->current_handler) {
		type_name = c->current_handler->output_type;
	}
	if (!type_name) {
		return;
	}
	char* json_str = csilk_json_marshal(type_name, ptr);
	if (json_str) {
		c->response.status = status;
		csilk_set_header(c, "Content-Type", "application/json");
		if (c->response.body && c->response.body_is_managed) {
			free((void*)c->response.body);
		}
		c->response.body = json_str;
		c->response.body_len = strlen(json_str);
		c->response.body_is_managed = 1;
	}
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
		char* value = NULL;

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
			pos = NULL;
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
	const char* body = csilk_get_body(c, NULL);
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
		char* value = NULL;

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
			pos = NULL;
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
 * @return The URL-decoded field value, or NULL if not found.
 * @note The returned pointer lives in arena memory. */
const char*
csilk_get_form_field(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return NULL;
	}
	return map_get(&c->request.form_params, key);
}

/** @brief libuv write completion callback for streaming (non-terminal) writes.
 *
 * Frees the write buffer and request structure. On error, logs to stderr.
 * Does NOT close the connection — terminal writes use on_stream_end_write().
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (negative indicates error). */
static void
on_stream_write(uv_write_t* req, int status)
{
	if (status < 0) {
		fprintf(stderr, "Stream write error %s\n", uv_strerror(status));
	}
	if (req->data) {
		free(req->data);
	}
	free(req);
}

/** @brief Check if the client requested "Connection: close" in the request.
 *
 * Examines the "Connection" request header for a value of "close"
 * (case-insensitive).
 *
 * @param c The request context.
 * @return 1 if the client requested close, 0 otherwise.
 * @note Used by send_chunked_headers() to determine the response's
 *       Connection header value. */
static int
client_wants_close(csilk_ctx_t* c)
{
	const char* connection = csilk_get_header(c, "Connection");
	return connection && strcasecmp(connection, "close") == 0;
}

/** @brief libuv write completion callback for a terminal chunk — closes the
 * connection.
 *
 * Frees the write buffer and request structure, then closes the underlying
 * handle. This is used for the final chunk of a streaming response.
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (negative indicates error). */
static void
on_stream_end_write(uv_write_t* req, int status)
{
	if (status < 0) {
		fprintf(stderr, "Stream end write error %s\n", uv_strerror(status));
	}
	if (req->data) {
		free(req->data);
	}
	if (req->handle) {
		uv_close((uv_handle_t*)req->handle, NULL);
	}
	free(req);
}

/** @brief Send HTTP response headers with Transfer-Encoding: chunked.
 *
 * Constructs and sends the HTTP status line, chunked transfer-encoding
 * header, connection header (keep-alive or close), and all custom response
 * headers. This is automatically called on the first call to
 * csilk_response_write() if the response has not started yet.
 *
 * @param c Request context.
 * @return 0 on success, -1 on allocation failure or NULL input.
 * @note Sets c->response_started = 1 on success. */
static int
send_chunked_headers(csilk_ctx_t* c)
{
	if (!c || !c->_internal_client) {
		return -1;
	}

	int status = c->response.status ? c->response.status : CSILK_STATUS_OK;
	const char* status_text = status == CSILK_STATUS_OK		? "OK"
				  : status == CSILK_STATUS_CREATED	? "Created"
				  : status == CSILK_STATUS_BAD_REQUEST	? "Bad Request"
				  : status == CSILK_STATUS_UNAUTHORIZED ? "Unauthorized"
				  : status == CSILK_STATUS_FORBIDDEN	? "Forbidden"
				  : status == CSILK_STATUS_NOT_FOUND	? "Not Found"
				  : status == CSILK_STATUS_INTERNAL_SERVER_ERROR
				      ? "Internal Server Error"
				      : "OK";

	int want_close = client_wants_close(c);
	const char* conn_val = want_close ? "close" : "keep-alive";

	size_t custom_headers_len = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
			custom_headers_len += h->key_len + 2 + h->value_len + 2;
		}
	}

	int header_len = snprintf(NULL,
				  0,
				  "HTTP/1.1 %d %s\r\n"
				  "Transfer-Encoding: chunked\r\n"
				  "Connection: %s\r\n",
				  status,
				  status_text,
				  conn_val);
	if (header_len < 0) {
		return -1;
	}

	size_t response_len = (size_t)header_len + custom_headers_len + 2;
	uv_write_t* req = malloc(sizeof(uv_write_t));
	if (!req) {
		return -1;
	}

	char* write_base = malloc(response_len + 1);
	if (!write_base) {
		free(req);
		return -1;
	}

	int pos = snprintf(write_base,
			   response_len + 1,
			   "HTTP/1.1 %d %s\r\n"
			   "Transfer-Encoding: chunked\r\n"
			   "Connection: %s\r\n",
			   status,
			   status_text,
			   conn_val);

	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
			pos += snprintf(write_base + pos,
					response_len + 1 - (size_t)pos,
					"%s: %s\r\n",
					h->key,
					h->value);
		}
	}

	snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n");

	uv_buf_t buf = uv_buf_init(write_base, (size_t)pos + 2);
	req->data = write_base;
	uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
	uv_write(req, stream, &buf, 1, on_stream_write);
	c->response_started = 1;
	return 0;
}

/** @brief Write a single chunked transfer frame: [hex-size]\\r\\n[data]\\r\\n.
 *
 * Formats the data length as a hex string, prepends it, appends the trailing
 * CRLF, and sends the complete frame via _csilk_send_data(). The frame buffer
 * is heap-allocated and freed after sending.
 *
 * @param c    Request context.
 * @param data Payload data for this chunk.
 * @param len  Length of payload in bytes.
 * @note The terminal chunk (zero-length) should be sent via
 * csilk_response_end(). */
static void
write_chunk_frame(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
	char size_buf[32];
	int size_len = snprintf(size_buf, sizeof(size_buf), "%zx\r\n", len);
	if (size_len <= 0) {
		return;
	}

	size_t total = (size_t)size_len + len + 2;
	char* buf = malloc(total);
	if (!buf) {
		return;
	}

	memcpy(buf, size_buf, (size_t)size_len);
	if (len > 0 && data) {
		memcpy(buf + (size_t)size_len, data, len);
	}
	buf[(size_t)size_len + len] = '\r';
	buf[(size_t)size_len + len + 1] = '\n';

	_csilk_send_data(c, (const uint8_t*)buf, total);
	free(buf);
}

/** @brief Write data to a streaming response using chunked transfer encoding.
 *
 * On the first call, automatically sends chunked headers (status line +
 * Transfer-Encoding: chunked). Subsequent calls append data chunks.
 * Sets the response to async mode so the framework does not auto-send
 * the response after the handler returns.
 *
 * @param c    Request context.
 * @param data Payload data to write.
 * @param len  Length of data in bytes.
 * @note After all data has been written, call csilk_response_end() to send
 *       the terminal chunk and finalize the response.
 * @note Calling with len=0 is a no-op. */
void
csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
	if (!c || !c->_internal_client) {
		return;
	}

	uv_stream_t* stream = (uv_stream_t*)c->_internal_client;

	if (!c->response_started) {
		if (send_chunked_headers(c) != 0) {
			return;
		}
		c->response_started = 1;
		c->is_async = 1;
	}

	if (len == 0) {
		return;
	}
	write_chunk_frame(c, data, len);
}

/** @brief Finalize a streaming response by sending the terminal chunk.
 *
 * If the response has not yet started, sends chunked headers first.
 * Then sends the zero-length terminal chunk ("0\\r\\n\\r\\n") which signals
 * to the client that the stream is complete.
 *
 * @param c Request context.
 * @note Must be called after all csilk_response_write() calls are done.
 *       Safe to call even if response_started is false. */
void
csilk_response_end(csilk_ctx_t* c)
{
	if (!c || !c->_internal_client) {
		return;
	}

	uv_stream_t* stream = (uv_stream_t*)c->_internal_client;

	if (!c->response_started) {
		send_chunked_headers(c);
		c->is_async = 1;
	}

	/* Terminal chunk: 0\r\n\r\n */
	_csilk_send_data(c, (const uint8_t*)"0\r\n\r\n", 5);
}
