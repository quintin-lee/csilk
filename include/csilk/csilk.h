/**
 * @file csilk.h
 * @brief High-performance C web framework — main public API header.
 *
 * Defines all public types, enums, macros, and function declarations
 * for the csilk HTTP web framework, including the request context,
 * router, server, middleware, WebSocket, SSE, and utility APIs.
 * Inspired by Gin (Golang).
 * @version 0.2.3
 * @copyright MIT License
 */

#ifndef CSILK_H
#define CSILK_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "cJSON.h"

/* Forward declarations to break circular dependency:
   csilk/drivers/db.h includes csilk.h, so types defined in db.h
   must be forward-declared here before the include. */
typedef struct csilk_db_pool_s csilk_db_pool_t;

#include "csilk/drivers/ai.h"
#include "csilk/drivers/cipher.h"
#include "csilk/drivers/db.h"
#include "csilk/drivers/perm.h"
#include "csilk/reflection/reflect.h"

/**
 * @brief Csilk framework version string (MAJOR.MINOR.PATCH).
 * Used for identification in logs, headers, and the OpenAPI spec.
 */
#define CSILK_VERSION "0.2.3"

/**
 * @brief Maximum number of URL path parameters that can be extracted from a
 * single request.  Parameters beyond this limit are silently ignored.
 * Tune if your routes contain more than 20 dynamic segments.
 */
#define CSILK_MAX_PARAMS 20

/**
 * @brief Maximum number of items that can be stored in the context key-value storage.
 *
 * This limit prevents uncontrolled memory consumption in the request arena
 * by preventing a single request from setting an excessive number of keys.
 */
#define CSILK_MAX_STORAGE 64

/** @name HTTP Status Codes
 *  Standardized macros for common HTTP response status codes.
 *  Use these instead of raw integer literals for readability.
 *  @{ */
#define CSILK_STATUS_CONTINUE 100
#define CSILK_STATUS_SWITCHING_PROTOCOLS 101
#define CSILK_STATUS_OK 200
#define CSILK_STATUS_CREATED 201
#define CSILK_STATUS_NO_CONTENT 204
#define CSILK_STATUS_PARTIAL_CONTENT 206
#define CSILK_STATUS_MOVED_PERMANENTLY 301
#define CSILK_STATUS_FOUND 302
#define CSILK_STATUS_NOT_MODIFIED 304
#define CSILK_STATUS_TEMPORARY_REDIRECT 307
#define CSILK_STATUS_BAD_REQUEST 400
#define CSILK_STATUS_UNAUTHORIZED 401
#define CSILK_STATUS_PAYMENT_REQUIRED 402
#define CSILK_STATUS_FORBIDDEN 403
#define CSILK_STATUS_NOT_FOUND 404
#define CSILK_STATUS_METHOD_NOT_ALLOWED 405
#define CSILK_STATUS_REQUEST_TIMEOUT 408
#define CSILK_STATUS_CONFLICT 409
#define CSILK_STATUS_GONE 410
#define CSILK_STATUS_PAYLOAD_TOO_LARGE 413
#define CSILK_STATUS_RANGE_NOT_SATISFIABLE 416
#define CSILK_STATUS_URI_TOO_LONG 414
#define CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE 415
#define CSILK_STATUS_TOO_MANY_REQUESTS 429
#define CSILK_STATUS_INTERNAL_SERVER_ERROR 500
#define CSILK_STATUS_NOT_IMPLEMENTED 501
#define CSILK_STATUS_BAD_GATEWAY 502
#define CSILK_STATUS_SERVICE_UNAVAILABLE 503
#define CSILK_STATUS_GATEWAY_TIMEOUT 504
/** @} */

/**
 * @brief Opaque request context type.
 * Created per-request by the server and passed to every handler and middleware.
 * Carries the request, response, path parameters, arena allocator, storage,
 * and connection state (WebSocket/SSE). Not thread-safe — access only from the
 * libuv loop thread that owns the connection.
 */
typedef struct csilk_ctx_s csilk_ctx_t;

/**
 * @brief Pluggable storage driver for context key-value pairs.
 *
 * Allows users to replace the default in-memory arena-backed store with a
 * custom backend (e.g., a thread-local or external cache). Every function
 * receives the owning csilk_ctx_t so drivers can access per-request state.
 *
 * @note All driver functions are called from the libuv event-loop thread;
 *       implementations need not be thread-safe.
 */
typedef struct {
	/** @brief Store a value associated with @p key.
   *  @param c  Owning request context.
   *  @param key  NUL-terminated key string (copied internally).
   *  @param value  Opaque pointer to store. Ownership remains with caller. */
	void (*set)(csilk_ctx_t* c, const char* key, void* value);
	/** @brief Retrieve a value by key.
   *  @param c  Owning request context.
   *  @param key  NUL-terminated key string.
   *  @return The stored pointer, or NULL if @p key was never set. */
	void* (*get)(csilk_ctx_t* c, const char* key);
	/** @brief Clear all stored key-value pairs.
   *  Called during csilk_ctx_cleanup to release references. */
	void (*clear)(csilk_ctx_t* c);
} csilk_storage_driver_t;

/**
 * @brief Function pointer for route handlers and middleware.
 *
 * Every handler receives the per-request context and operates on it
 * (reading request data, setting response data, calling csilk_next to
 * pass control to the next handler in the chain, etc.).
 *
 * @param c  The per-request context.
 */
typedef void (*csilk_handler_t)(csilk_ctx_t* c);

/**
 * @brief A single HTTP header stored as a node in a chained hash table.
 *
 * Key and value are NUL-terminated strings allocated from the request arena.
 * The @p next pointer forms a singly-linked list for hash-collision chains.
 *
 * @note Strings are NOT individually freeable — they live until the arena
 *       is destroyed in csilk_ctx_cleanup.
 */
typedef struct csilk_header_s {
	char* key;	  /**< NUL-terminated header field name (lowercased for
                  case-insensitive lookup). */
	char* value;	  /**< NUL-terminated header field value (raw, as received or set).
                */
	size_t key_len;	  /**< Cached strlen(@p key) for rapid comparison. */
	size_t value_len; /**< Cached strlen(@p value). */
	struct csilk_header_s* next; /**< Pointer to the next header in the same hash
                                  bucket (collision chain). */
} csilk_header_t;

/**
 * @brief Number of buckets in the header chained hash table.
 *
 * Larger values reduce collision chains at the cost of a small amount of
 * per-map memory.  Override at compile-time with -DCSILK_HEADER_BUCKETS=N.
 * @note Must be a power of two for efficient bucket indexing.
 */
#ifndef CSILK_HEADER_BUCKETS
#define CSILK_HEADER_BUCKETS 64
#endif

/**
 * @brief A fixed-size chained hash table for HTTP headers.
 *
 * Uses CSILK_HEADER_BUCKETS buckets; each bucket holds a singly-linked list
 * of csilk_header_t nodes.  Used for both request and response headers,
 * query parameters, and form fields.
 *
 * @note Not thread-safe — all mutations occur on the event-loop thread.
 */
typedef struct csilk_header_map_s {
	csilk_header_t* buckets[CSILK_HEADER_BUCKETS]; /**< Bucket array; each entry points to the
                                        head of a collision chain (or NULL). */
} csilk_header_map_t;

/**
 * @brief Parsed HTTP request.
 *
 * Populated by the HTTP parser before handlers are invoked.  All string
 * fields point into arena-allocated memory that stays valid until
 * csilk_ctx_cleanup.
 */
typedef struct {
	char* method;	 /**< HTTP method verb (e.g., "GET", "POST", "DELETE"). */
	char* path;	 /**< Decoded URL path (percent-encoding removed, query string
                   stripped). */
	char* body;	 /**< Raw request body bytes, or NULL for requests without a body. */
	size_t body_len; /**< Number of bytes in @p body. */
	csilk_header_map_t headers;	 /**< Hash map of request headers (keys lowercased
                                 for case-insensitive lookup). */
	csilk_header_map_t query_params; /**< Hash map of parsed query-string parameters. */
	csilk_header_map_t form_params;	 /**< Hash map of parsed application/x-www-form-urlencoded
                      fields (populated by csilk_parse_form_urlencoded). */
} csilk_request_t;

/**
 * @brief Mutable HTTP response.
 *
 * Handlers write their response into this struct.  The framework serialises
 * it after the handler chain completes (or when csilk_response_end is called
 * for streaming responses).
 */
typedef struct {
	int status;		    /**< HTTP status code (e.g., 200, 404, 500). Defaults to 200. */
	const char* body;	    /**< Response body content. If @p body_is_managed is 1 the
                       framework calls free() when done. */
	size_t body_len;	    /**< Byte length of @p body. */
	csilk_header_map_t headers; /**< Hash map of response headers to send. */
	int body_is_managed;	    /**< Non-zero if @p body was allocated with malloc() and
                          must be free()'d by the framework. */
} csilk_response_t;

/**
 * @brief A single URL path parameter extracted from a route pattern.
 *
 * For a route like `/users/:id/posts/:post_id`, two csilk_param_t entries
 * are generated: {"id", actual_id} and {"post_id", actual_post_id}.
 *
 * @note The key and value strings live in the request arena and are valid
 *       until csilk_ctx_cleanup.
 */
typedef struct {
	char* key;   /**< Parameter name as defined in the route pattern (e.g., "id"). */
	char* value; /**< Actual decoded value from the request URL. */
} csilk_param_t;

/**
 * @brief Opaque arena allocator type.
 *
 * Provides bump-allocation semantics: memory is allocated in large chunks
 * and individual allocations are never freed — the entire arena is reset
 * or freed at once.  Ideal for request-scoped allocations because it is
 * faster than malloc/free and produces zero fragmentation.
 *
 * @note Not thread-safe — each request should have its own arena.
 */
typedef struct csilk_arena_s csilk_arena_t;

/**
 * @brief Get the HTTP method of the current request.
 *
 * @param c  The request context.
 * @return A NUL-terminated string such as "GET", "POST", etc.
 *         The pointer is valid until csilk_ctx_cleanup.
 */
const char* csilk_get_method(csilk_ctx_t* c);

/**
 * @brief Get the decoded URL path of the current request.
 *
 * The path has percent-encoding removed and the query string stripped.
 *
 * @param c  The request context.
 * @return The path string (e.g., "/users/42"). Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_path(csilk_ctx_t* c);

/**
 * @brief Get the raw request body and its length.
 *
 * Only valid after the full body has been parsed.  Returns NULL for methods
 * that have no body (GET, HEAD, etc.) or when the body is empty.
 *
 * @param c        The request context.
 * @param[out] out_len  Optional pointer to receive the body length in bytes.
 *                      May be NULL if the caller does not need the length.
 * @return Pointer to the raw body data (not NUL-terminated), or NULL if no
 *         body is present.
 */
const char* csilk_get_body(csilk_ctx_t* c, size_t* out_len);

/**
 * @brief Get the length of the raw request body.
 *
 * Convenience shortcut for csilk_get_body(c, &len) when only the length
 * is needed.
 *
 * @param c  The request context.
 * @return Body length in bytes, or 0 if no body is present.
 */
size_t csilk_get_body_len(csilk_ctx_t* c);

/**
 * @brief Check whether the connection has been upgraded to WebSocket.
 *
 * Returns 1 only after a successful csilk_ws_handshake call.
 *
 * @param c  The request context.
 * @return 1 if WebSocket mode is active, 0 otherwise.
 */
int csilk_is_websocket(csilk_ctx_t* c);

/**
 * @name WebSocket Room Management (MQ-based)
 * High-concurrency room broadcasting system leveraging the Message Queue.
 * @{ */

/** @brief Join a WebSocket client to a room.
 *  @param c          Request context (must be a WebSocket).
 *  @param room_name  Name of the room to join. */
void csilk_ws_join_room(csilk_ctx_t* c, const char* room_name);

/** @brief Remove a WebSocket client from a room.
 *  @param c          Request context.
 *  @param room_name  Name of the room to leave. */
void csilk_ws_leave_room(csilk_ctx_t* c, const char* room_name);

/** @brief Broadcast a message to all WebSockets in a room via MQ.
 *  @param c          Request context (used to access the server's MQ).
 *  @param room_name  Room to broadcast to.
 *  @param message    NUL-terminated message string. */
void csilk_ws_broadcast_room(csilk_ctx_t* c, const char* room_name, const char* message);

/** @} */

/**
 * @brief Enable or disable WebSocket mode.
 *
 * @param c             The request context.
 * @param is_websocket  1 to enable, 0 to disable.
 */
void csilk_set_websocket(csilk_ctx_t* c, int is_websocket);

/**
 * @brief Check whether the connection is in Server-Sent Events mode.
 *
 * Returns 1 only after csilk_sse_init has been called successfully.
 *
 * @param c  The request context.
 * @return 1 if SSE mode is active, 0 otherwise.
 */
int csilk_is_sse(csilk_ctx_t* c);

/**
 * @brief Enable or disable Server-Sent Events (SSE) mode.
 *
 * When SSE mode is active the framework will not automatically close
 * the connection after the handler returns.
 *
 * @param c       The request context.
 * @param is_sse  1 to enable SSE mode, 0 to disable.
 */
void csilk_set_sse(csilk_ctx_t* c, int is_sse);

/**
 * @brief Check whether the handler chain has been aborted.
 *
 * Handlers can check this after calling csilk_next to see if a downstream
 * handler or middleware called csilk_abort.
 *
 * @param c  The request context.
 * @return 1 if csilk_abort was called, 0 otherwise.
 */
int csilk_is_aborted(csilk_ctx_t* c);

/**
 * @brief Register a callback for incoming WebSocket messages.
 *
 * Must be called after csilk_ws_handshake and before the event loop
 * delivers data.  The callback receives the context, payload bytes,
 * payload length, and the WebSocket opcode.
 *
 * @param c   The request context.
 * @param cb  Callback function invoked for each received message.
 *            The callback must not block; it runs on the event-loop thread.
 */
void csilk_set_on_ws_message(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode));

/** @brief Set a callback for outgoing WebSocket frames (for testing). */
void csilk_set_on_ws_send(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode));

/**
 * @brief Get the currently registered WebSocket message callback.
 *
 * @param c  The request context.
 * @return The callback function pointer, or NULL if none is set.
 */
void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t* c,
						const uint8_t* payload,
						size_t len,
						int opcode);

/**
 * @brief Get the unique identifier for the current request.
 *
 * The ID is auto-generated (UUID v4) by the csilk_request_id_middleware
 * or by the server if no middleware is installed.
 *
 * @param c  The request context.
 * @return A NUL-terminated UUID string (e.g.,
 * "f81d4fae-7dec-11d0-a765-00a0c91e6bf6"). Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_request_id(csilk_ctx_t* c);

/**
 * @brief Get the arena allocator associated with the request context.
 *
 * Use this for all short-lived per-request allocations.  Memory is
 * automatically reclaimed in csilk_ctx_cleanup and does not need
 * individual free() calls.
 *
 * @param c  The request context.
 * @return Pointer to the arena allocator.
 */
csilk_arena_t* csilk_get_arena(csilk_ctx_t* c);

/**
 * @brief Get the current response status code.
 *
 * @param c  The request context.
 * @return The HTTP status code (e.g., 200, 404).  Default is 200 if not set.
 */
int csilk_get_status(csilk_ctx_t* c);

/**
 * @brief Enable or disable asynchronous response mode.
 *
 * When async mode is enabled the framework will NOT automatically flush the
 * response after the handler chain returns.  The handler is responsible for
 * calling csilk_response_write / csilk_response_end at a later time (e.g.,
 * after an async I/O operation completes).
 *
 * @param c         The request context.
 * @param is_async  1 to enable async mode, 0 to disable (default).
 */
void csilk_set_async(csilk_ctx_t* c, int is_async);

/**
 * @brief Check whether asynchronous response mode is enabled.
 *
 * @param c  The request context.
 * @return 1 if async mode is active, 0 if the framework owns response flushing.
 */
int csilk_is_async(csilk_ctx_t* c);

/**
 * @brief Get the index of the currently executing handler in the chain.
 *
 * @param c  The request context.
 * @return Index (0-based) or -1 if the chain hasn't started.
 */
int csilk_get_handler_index(csilk_ctx_t* c);

/**
 * @brief Set the request unique identifier.
 *
 * @param c   The request context.
 * @param id  The new request ID string. It is copied into the context.
 */
void csilk_set_request_id(csilk_ctx_t* c, const char* id);

/**
 * @brief Get the libuv work request associated with the context.
 *
 * Use this to offload long-running operations to the thread pool while
 * maintaining context state.
 *
 * @param c  The request context.
 * @return Pointer to the context's internal uv_work_t.
 */
uv_work_t* csilk_get_work_req(csilk_ctx_t* c);

/**
 * @brief Set the zero-copy file response parameters.
 *
 * Configures the context to send a file using platform-native zero-copy
 * mechanisms (e.g., sendfile). This should be used in conjunction with
 * setting the response as asynchronous.
 *
 * @param c       The request context.
 * @param fd      Open file descriptor (O_RDONLY).
 * @param offset  Starting byte offset in the file.
 * @param size    Number of bytes to send.
 */
void csilk_set_file_response(csilk_ctx_t* c, int fd, size_t offset, size_t size);

/**
 * @brief Get the current zero-copy file descriptor.
 *
 * @param c  The request context.
 * @return The file descriptor, or -1 if no file response is configured.
 */
int csilk_get_file_fd(csilk_ctx_t* c);

/**
 * @brief Get the route pattern for the matched handler (e.g., "/users/:id").
 *
 * @param c  The request context.
 * @return The route path pattern, or NULL if no route was matched.
 */
const char* csilk_ctx_get_handler_path(csilk_ctx_t* c);

/**
 * @brief Get the permission string required by the matched handler.
 *
 * @param c  The request context.
 * @return The permission identifier, or NULL if none is required.
 */
const char* csilk_ctx_get_handler_perm_required(csilk_ctx_t* c);

/**
 * @brief Get the resource pattern for the matched handler's permission check.
 *
 * @param c  The request context.
 * @return The resource pattern, or NULL.
 */
const char* csilk_ctx_get_handler_perm_resource(csilk_ctx_t* c);

/**
 * @brief Overwrite the response body from middleware.
 *
 * Useful for middleware that transforms the response (e.g., gzip compression,
 * response transformation).  If @p managed is 1 the framework takes ownership
 * and calls free() when the response is sent.
 *
 * @param c       The request context.
 * @param body    Pointer to the new body data.
 * @param len     Byte length of @p body.
 * @param managed Ownership flag: 1 = framework calls free(@p body) when done,
 *                0 = caller retains ownership and body must stay valid until
 *                the response is sent.
 */
void csilk_set_response_body(csilk_ctx_t* c, const char* body, size_t len, int managed);

const char* csilk_get_response_body(csilk_ctx_t* c, size_t* out_len);

/**
 * @brief Send an HTTP redirect response with a custom status code.
 *
 * Sets the Location header and the response body to a minimal HTML
 * redirect page.  The handler chain is aborted after this call.
 *
 * @param c        The request context.
 * @param status   HTTP redirect status (e.g., 301 Moved Permanently,
 *                 302 Found, 307 Temporary Redirect).
 * @param location The destination URL.  Must not be NULL.
 */
void csilk_redirect(csilk_ctx_t* c, int status, const char* location);

/**
 * @brief Send a simple 302 Found redirect.
 *
 * Convenience wrapper around csilk_redirect with status 302.
 *
 * @param c   The request context.
 * @param url The destination URL.
 */
void csilk_redirect_simple(csilk_ctx_t* c, const char* url);

/**
 * @brief Store an opaque value in the request context.
 *
 * The @p key string is duplicated into the request arena.  The @p value
 * pointer is stored as-is — the context does NOT take ownership.
 * The caller must ensure the pointed-to data remains valid at least until
 * csilk_ctx_cleanup.
 *
 * @param c     The request context.
 * @param key   NUL-terminated key name (a copy is made internally).
 * @param value Opaque pointer to store.  May be NULL (which will be returned
 *              by csilk_get, so storing NULL is indistinguishable from "not
 *              set" — avoid it).
 */
void csilk_set(csilk_ctx_t* c, const char* key, void* value);

/**
 * @brief Retrieve an opaque value from the request context.
 *
 * @param c   The request context.
 * @param key NUL-terminated key name.
 * @return The value pointer previously stored with csilk_set, or NULL if
 *         @p key was never set (or was explicitly set to NULL — see the
 *         note on csilk_set).
 */
void* csilk_get(csilk_ctx_t* c, const char* key);

/**
 * @brief Pass control to the next handler in the middleware/handler chain.
 *
 * Implements the "onion" model: handlers before csilk_next run on the way in,
 * handlers after csilk_next run on the way out (after downstream handlers).
 * Call csilk_is_aborted after returning to check whether a downstream handler
 * called csilk_abort.
 *
 * @param c  The request context.
 */
void csilk_next(csilk_ctx_t* c);

/**
 * @brief Immediately abort the handler chain.
 *
 * No further handlers or middleware run (except code after csilk_next in the
 * current handler that checks csilk_is_aborted).  The response accumulated
 * so far is sent to the client.
 *
 * @param c  The request context.
 */
void csilk_abort(csilk_ctx_t* c);

/**
 * @brief Set the HTTP response status code.
 *
 * @param c      The request context.
 * @param status The HTTP status code (e.g., 200, 404, 500).
 */
void csilk_status(csilk_ctx_t* c, int status);

/**
 * @brief Set a plain-text response body and status code.
 *
 * The @p msg string is copied into the request arena so the caller's
 * buffer can be reused immediately.  Equivalent to calling csilk_status
 * then setting the response body.
 *
 * @param c      The request context.
 * @param status The HTTP status code.
 * @param msg    The plain-text body string (NUL-terminated).
 */
void csilk_string(csilk_ctx_t* c, int status, const char* msg);

/**
 * @brief Get a URL path parameter by key.
 *
 * Parameters are extracted from the route pattern by the router.  For a route
 * `/users/:id`, csilk_get_param(c, "id") returns the actual value.
 *
 * @param c   The request context.
 * @param key The parameter name as defined in the route pattern (e.g., "id").
 * @return The decoded parameter value, or NULL if @p key is not a known
 *         parameter.  Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_param(csilk_ctx_t* c, const char* key);

/**
 * @brief Get the number of path parameters extracted from the URL.
 *
 * @param c  The request context.
 * @return Count of parameters (0..CSILK_MAX_PARAMS).
 */
int csilk_get_params_count(csilk_ctx_t* c);

/**
 * @brief Get the name of a path parameter by its index.
 *
 * @param c      The request context.
 * @param index  Index of the parameter (0..count-1).
 * @return The parameter name string, or NULL if index is out of bounds.
 */
const char* csilk_get_param_key(csilk_ctx_t* c, int index);

/**
 * @brief Get the value of a path parameter by its index.
 *
 * @param c      The request context.
 * @param index  Index of the parameter (0..count-1).
 * @return The parameter value string, or NULL if index is out of bounds.
 */
const char* csilk_get_param_value(csilk_ctx_t* c, int index);

/**
 * @brief Get a request header value by name (case-insensitive).
 *
 * @param c   The request context.
 * @param key The header field name (e.g., "Content-Type").
 * @return The header value string, or NULL if the header is not present.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_header(csilk_ctx_t* c, const char* key);

/**
 * @brief Get a response header value by name (case-insensitive).
 *
 * @param c   The request context.
 * @param key The header field name.
 * @return The header value string, or NULL if the header has not been set.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_response_header(csilk_ctx_t* c, const char* key);

/**
 * @brief Get a query-string parameter by key.
 *
 * Only works after the request has been fully parsed (always true in
 * handlers).  The first value is returned when a key appears multiple times.
 *
 * @param c   The request context.
 * @param key The query parameter name.
 * @return The parameter value, or NULL if not present.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_query(csilk_ctx_t* c, const char* key);

/**
 * @brief Parse the request body as application/x-www-form-urlencoded.
 *
 * Populates the form_params hash map in the request.  After calling this,
 * form fields can be retrieved with csilk_get_form_field.  Safe to call
 * multiple times — subsequent calls are no-ops.
 *
 * @param c  The request context.
 */
void csilk_parse_form_urlencoded(csilk_ctx_t* c);

/**
 * @brief Get a form-urlencoded field value by key.
 *
 * Only returns meaningful data after csilk_parse_form_urlencoded has been
 * called.
 *
 * @param c   The request context.
 * @param key The form field name.
 * @return The field value, or NULL if not found.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_form_field(csilk_ctx_t* c, const char* key);

/**
 * @brief Set (or overwrite) a request header.
 *
 * The key and value are copied into the request arena.
 *
 * @param c     The request context.
 * @param key   The header field name.
 * @param value The header field value.
 */
void csilk_set_request_header(csilk_ctx_t* c, const char* key, const char* value);

/**
 * @brief Set (or overwrite) a response header.
 *
 * If the header already exists its value is replaced.  Key and value are
 * copied into the request arena.
 *
 * @param c     The request context.
 * @param key   The header field name.
 * @param value The header field value.
 */
void csilk_set_header(csilk_ctx_t* c, const char* key, const char* value);

/**
 * @brief Append a response header, preserving any existing value(s).
 *
 * Unlike csilk_set_header, this adds another entry rather than replacing
 * the existing one.  Useful for headers like Set-Cookie that may appear
 * multiple times.
 *
 * @param c     The request context.
 * @param key   The header field name.
 * @param value The header field value to append.
 */
void csilk_add_header(csilk_ctx_t* c, const char* key, const char* value);

/**
 * @brief Release all memory and resources associated with a request context.
 *
 * Frees the arena allocator, destroys header hash tables, and resets the
 * context struct.  Called automatically by the framework after the response
 * is sent.  Not intended for direct use in user code.
 *
 * @param c  The request context to clean up.
 */
void csilk_ctx_cleanup(csilk_ctx_t* c);

/**
 * @brief Create a new arena allocator.
 *
 * The arena allocates memory in fixed-size chunks and hands out bump-allocated
 * blocks.  When the current chunk is exhausted a new one is allocated.
 *
 * @param default_chunk_size  Initial chunk size in bytes.  Pass 0 for a
 *                            sensible default (typically 4–8 KB).
 * @return Pointer to the new arena, or NULL if malloc fails.
 */
csilk_arena_t* csilk_arena_new(size_t default_chunk_size);

/**
 * @brief Allocate zero-initialised memory from an arena.
 *
 * The returned memory is valid until csilk_arena_free, csilk_arena_reset, or
 * csilk_ctx_cleanup.  No individual free() is required.
 *
 * @param arena  The arena allocator.
 * @param size   Number of bytes to allocate.
 * @return Pointer to the allocated block (always suitably aligned), or NULL
 *         if the allocation failed (the arena's malloc failed).
 */
void* csilk_arena_alloc(csilk_arena_t* arena, size_t size);

/**
 * @brief Duplicate a NUL-terminated string using the arena allocator.
 *
 * @param arena  The arena allocator.
 * @param s      Source string to duplicate.  Must be NUL-terminated.
 * @return A copy of @p s allocated from @p arena, or NULL on allocation
 *         failure.  If @p s is NULL the behaviour is undefined.
 */
char* csilk_arena_strdup(csilk_arena_t* arena, const char* s);

/**
 * @brief Duplicate @p n bytes of a string using the arena allocator.
 *
 * @param arena  The arena allocator.
 * @param s      Source string to duplicate.
 * @param n      Number of bytes to copy.
 * @return A copy of @p n bytes of @p s allocated from @p arena, or NULL on
 *         allocation failure.  If @p s is NULL the behaviour is undefined.
 */
char* csilk_arena_strndup(csilk_arena_t* arena, const char* s, size_t n);

/**
 * @brief Free all memory chunks owned by the arena.
 *
 * After this call the arena pointer is invalid and must not be used again.
 *
 * @param arena  The arena allocator to destroy.
 */
void csilk_arena_free(csilk_arena_t* arena);

/**
 * @brief Reset the arena without freeing its chunks.
 *
 * The arena can be reused after a reset — subsequent allocations reuse
 * the existing chunk memory.  Useful for pooling arenas across requests
 * to reduce malloc pressure.
 *
 * @param arena  The arena allocator to reset.
 */
void csilk_arena_reset(csilk_arena_t* arena);
void csilk_arena_get_stats(csilk_arena_t* arena, size_t* total_size, size_t* total_used);

/**
 * @brief Panic-recovery middleware.
 *
 * Wraps the handler chain in a setjmp/longjmp boundary.  If a handler calls
 * csilk_panic (or a segfault occurs within the protected scope), this
 * middleware sends a 500 response and logs the error instead of crashing the
 * server.
 *
 * Should be registered as the outermost middleware.
 *
 * @param c  The request context.
 */
void csilk_recovery_handler(csilk_ctx_t* c);

/**
 * @brief Trigger a panic (caught by recovery middleware).
 *
 * Calls longjmp back to the setjmp point established by
 * csilk_recovery_handler.  If no recovery middleware is registered the
 * behaviour is undefined (likely a crash).
 *
 * @param c  The request context.
 */
void csilk_panic(csilk_ctx_t* c);

/**
 * @brief Log severity levels.
 *
 * Levels are ordered: messages at or above the configured minimum level are
 * emitted.  CSILK_LOG_FATAL terminates the process after logging.
 */
typedef enum {
	CSILK_LOG_TRACE, /**< Finest-grained diagnostic messages (development only).
                    */
	CSILK_LOG_DEBUG, /**< Debugging information useful during development. */
	CSILK_LOG_INFO,	 /**< Normal operational messages (e.g., request completed). */
	CSILK_LOG_WARN,	 /**< Warning conditions that are not errors (e.g., slow
                     request). */
	CSILK_LOG_ERROR, /**< Error conditions that still allow the server to
                      continue. */
	CSILK_LOG_FATAL	 /**< Fatal errors; the server will exit after logging. */
} csilk_log_level_t;

/**
 * @brief Logger initialisation configuration.
 *
 * Controls log output destination, formatting, level filtering, and rotation.
 * Passed by value (not pointer) to csilk_log_init.
 */
typedef struct {
	csilk_log_level_t level; /**< Minimum level to emit (messages below this are
                              filtered out). */
	const char* file_path;	 /**< Path to the log file, or NULL to log to stderr. */
	size_t max_file_size;	 /**< Maximum file size in bytes before rotation (0 =
                        rotation disabled). Requires @p file_path to be set. */
	int use_colors;		 /**< Enable ANSI colour escape codes: 1 = on, 0 = off, -1 =
                        auto-detect (default). */
	int json_format;	 /**< When non-zero, emit newline-delimited JSON records
                        instead of human-readable lines. */
} csilk_log_config_t;

/** @brief Initialize the global logger with config.
 * @param config Logger configuration.
 * @return 0 on success, -1 on failure. */
int csilk_log_init(csilk_log_config_t config);

/** @brief Internal log function (use macros instead).
 * @param lv Log severity level.
 * @param file Source file name (__FILE__).
 * @param line Source line number (__LINE__).
 * @param func Function name (__func__).
 * @param fmt Printf-style format string.
 * @param ... Format arguments. */
void _csilk_log_internal(
    csilk_log_level_t lv, const char* file, int line, const char* func, const char* fmt, ...);

/** @brief Close the global logger. */
void csilk_log_close();

/** @brief Log a structured JSON message with extra key-value fields.
 *
 * Produces a JSON log line with the standard fields
 * (time/level/file/line/func/msg) plus any fields in @p extra.  If json_format
 * is off this behaves like a normal log line (extra fields are ignored).
 *
 * @param lv    Log severity level.
 * @param file    Source file name (__FILE__).
 * @param line    Source line number (__LINE__).
 * @param func    Function name (__func__).
 * @param extra   cJSON object with extra structured fields (can be NULL).
 *                Ownership is taken — do not use after the call.
 * @param fmt     Printf-style format string for the log message.
 * @param ...     Format arguments. */
void _csilk_log_structured(csilk_log_level_t lv,
			   const char* file,
			   int line,
			   const char* func,
			   cJSON* extra,
			   const char* fmt,
			   ...);

/** @brief Check whether the logger is in JSON format mode.
 * @return 1 if json_format is enabled, 0 otherwise. */
int csilk_log_is_json(void);

/** @brief Set the Request ID for the current thread (for log correlation).
 * @param request_id The Request ID string, or NULL to clear. */
void csilk_log_set_request_id(const char* request_id);

/** @brief Create a simple key-value cJSON object for structured logging.
 *
 * Convenience helper that builds a cJSON object from alternating key/value
 * string pairs terminated by a NULL key.
 *
 * @code
 *   cJSON* fields = csilk_log_make_kv("method", method, "path", path, NULL);
 *   _csilk_log_structured(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, fields,
 *                         "request completed");
 * @endcode
 *
 * @param key   First key.
 * @param ...   Value, then key, value, ... terminated by NULL.
 * @return New cJSON object (caller owns). */
cJSON* csilk_log_make_kv(const char* key, ...);

/** @name Logging Macros
 * Convenience macros that capture source location.
 * @{ */
/** @brief Log a TRACE-level message. */
#define CSILK_LOG_T(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a DEBUG-level message. */
#define CSILK_LOG_D(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log an INFO-level message. */
#define CSILK_LOG_I(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a WARN-level message. */
#define CSILK_LOG_W(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log an ERROR-level message. */
#define CSILK_LOG_E(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a FATAL-level message. */
#define CSILK_LOG_F(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

/** @brief Log a structured JSON message (only meaningful when json_format is
 * on).
 * @param level Log level.
 * @param extra  cJSON* with extra fields (can be NULL).
 * @param ...    printf-style format and args for the message string. */
#define CSILK_LOG_STRUCT(level, extra, ...)                                                        \
	_csilk_log_structured(level, __FILE__, __LINE__, __func__, extra, __VA_ARGS__)
/** @} */

/** @brief Logging middleware handler.
 * Logs request method, path, and processing time.
 * @param c The request context. */
void csilk_logger_handler(csilk_ctx_t* c);

/** @brief Request ID middleware.
 * Generates a unique ID for each request and sets X-Request-Id header.
 * @param c The request context. */
void csilk_request_id_middleware(csilk_ctx_t* c);

/** @brief Built-in Health Check handler.
 * Returns a simple JSON response {"status": "up"}.
 * @param c The request context. */
void csilk_health_check_handler(csilk_ctx_t* c);

/** @brief Built-in Readiness Check handler.
 * Performs deep health check (MQ, connections) and returns 200 or 503.
 * @param c The request context. */
void csilk_ready_check_handler(csilk_ctx_t* c);

/**
 * @brief CORS middleware configuration.
 *
 * Maps directly to the Access-Control-* response headers.  Strings are used
 * as-is — the caller must ensure they remain valid for the lifetime of the
 * middleware.
 */
typedef struct {
	const char* allow_origin;  /**< Value of the Access-Control-Allow-Origin header
                               (e.g., "*" or "https://example.com"). */
	const char* allow_methods; /**< Value of the Access-Control-Allow-Methods
                                header (e.g., "GET, POST, PUT, DELETE"). */
	const char* allow_headers; /**< Value of the Access-Control-Allow-Headers
                                header (e.g., "Content-Type, Authorization"). */
	int allow_credentials;	   /**< Non-zero to include
                                Access-Control-Allow-Credentials: true. */
	int max_age;		   /**< Value of Access-Control-Max-Age in seconds (e.g., 86400 for
                  24 h). */
} csilk_cors_config_t;

/**
 * @brief CORS middleware — handles preflight and adds CORS headers.
 *
 * Must be called as a route or group middleware.  For preflight OPTIONS
 * requests the middleware sends the appropriate headers and aborts the chain
 * (status 204).  For other requests the CORS headers are added and the chain
 * continues.
 *
 * @param c      The request context.
 * @param config CORS settings.  Must remain valid for the call duration.
 */
void csilk_cors_middleware(csilk_ctx_t* c, const csilk_cors_config_t* config);

/**
 * @brief Simple per-IP rate-limiting middleware.
 *
 * Uses a fixed-window counter per client IP.  If the limit is exceeded a
 * 429 Too Many Requests response is sent and the handler chain is aborted.
 *
 * @param c     The request context.
 * @param limit Maximum number of requests allowed per minute for a single IP.
 */
void csilk_rate_limit_middleware(csilk_ctx_t* c, int limit);

/**
 * @brief Stateless CSRF protection middleware.
 *
 * Checks for a valid CSRF token in the request (via header or form field)
 * on state-changing methods (POST, PUT, DELETE, PATCH).  If the token is
 * missing or invalid the chain is aborted with 403 Forbidden.
 *
 * @param c  The request context.
 */
void csilk_csrf_middleware(csilk_ctx_t* c);

/**
 * @brief Security layer statistics.
 */
typedef struct {
	uint64_t rate_limit_blocks; /**< Total requests blocked by rate limiter. */
	uint64_t csrf_violations;   /**< Total CSRF token validation failures. */
	uint64_t auth_failures;	    /**< Total failed authentication attempts. */
} csilk_security_stats_t;

/**
 * @brief OS-level process statistics.
 */
typedef struct {
	size_t rss_bytes;	  /**< Resident Set Size memory in bytes. */
	double cpu_user_time_sec; /**< CPU time spent in user mode. */
	double cpu_sys_time_sec;  /**< CPU time spent in kernel mode. */
} csilk_process_stats_t;

void csilk_security_get_stats(csilk_security_stats_t* stats);
void csilk_process_get_stats(csilk_process_stats_t* stats);

/**
 * @brief Generate a cryptographically random CSRF token.
 *
 * Produces a hex-encoded 32-byte (256-bit) random token.
 *
 * @param[out]  buf       Output buffer.  Must be at least 33 bytes for the
 *                        64-character hex string plus NUL terminator.
 * @param       buf_size  Size of @p buf in bytes.
 * @return 0 on success, -1 if @p buf_size is too small or the RNG fails.
 */
int csilk_csrf_generate_token(char* buf, size_t buf_size);

/**
 * @brief Low-level server configuration options.
 *
 * Controls timeouts, resource limits, TCP tuning, and TLS settings.
 * A zero-initialised struct provides safe defaults for most fields.
 * Apply via csilk_server_set_config before calling csilk_server_run.
 */
typedef struct csilk_server_config_s {
	unsigned int idle_timeout_ms;	 /**< HTTP keep-alive idle timeout in milliseconds.
                          Connection closed when no new request arrives within
                          this window. 0 = use default (typically 30 s). */
	unsigned int read_timeout_ms;	 /**< Maximum time in milliseconds to wait for the full
                          request headers+body (0 = disabled). */
	unsigned int write_timeout_ms;	 /**< Maximum time in milliseconds to send the
                                    response (0 = disabled). */
	unsigned int request_timeout_ms; /**< Maximum time in milliseconds for a complete
                               request/response cycle (0 = disabled). Overrides
                               read/write timeouts if set. */
	size_t max_body_size;		 /**< Maximum allowed request body size in bytes.
                               Requests exceeding this get 413 Payload Too Large. */
	size_t max_header_size;		 /**< Maximum total size of all request headers in
                               bytes. */
	size_t max_url_size;		 /**< Maximum URL length in bytes (0 = disabled). */
	size_t max_headers_count;	 /**< Maximum number of individual header fields (0 =
                               unlimited). */
	int max_connections;		 /**< Maximum concurrent client connections (0 =
                               unlimited). */
	int listen_backlog;		 /**< TCP listen(2) backlog hint passed to the kernel. */
	int tcp_nodelay;		 /**< Non-zero enables TCP_NODELAY (disable Nagle's
                         algorithm). */
	int tcp_keepalive;		 /**< TCP keep-alive probe interval in seconds (0 =
                         disabled). */
	int worker_threads;		 /**< Number of worker threads for SO_REUSEPORT listener
                         sockets. 0 = number of CPUs. */

	/* TLS configuration */
	int enable_tls;	     /**< Non-zero enables HTTPS via TLS.  Requires @p
                          tls_cert_file and @p tls_key_file. */
	char* tls_cert_file; /**< Path to the SSL/TLS certificate file (PEM format).
                          Must be set if @p enable_tls is 1. */
	char* tls_key_file;  /**< Path to the SSL/TLS private key file (PEM format).
                          Must be set if @p enable_tls is 1. */
	char* tls_ca_file;   /**< Path to the CA certificate bundle for
                          client-certificate verification (optional). */
	int tls_verify_peer; /**< Non-zero to require and verify a client certificate.
                          Requires @p tls_ca_file. */
} csilk_server_config_t;

/**
 * @brief Top-level application configuration.
 *
 * Unifies server, logger, middleware, and feature-flag settings.
 * Typically populated from a YAML file via csilk_load_config.
 */
typedef struct {
	int port;		      /**< TCP port the server listens on. */
	csilk_server_config_t server; /**< Low-level server/connection settings. */
	csilk_log_config_t logger;    /**< Logger initialisation settings. */
	struct {
		int enable;		    /**< Non-zero to install the CORS middleware. */
		csilk_cors_config_t config; /**< CORS header values when enabled. */
	} cors;				    /**< Cross-Origin Resource Sharing settings. */
	struct {
		int enable;		 /**< Non-zero to install the rate-limiter middleware. */
		int requests_per_minute; /**< Maximum requests/minute/IP when enabled. */
	} rate_limit;			 /**< Per-IP rate limiting settings. */
	struct {
		int enable;	/**< Non-zero to enable static file serving. */
		char* root_dir; /**< Absolute or relative path to the local directory to
                       serve. */
		char* prefix;	/**< URL prefix for static files (e.g., "/static"). */
	} static_files;		/**< Static file server settings. */
	struct {
		int enable_logger;   /**< Non-zero to install the request-logging middleware.
                        */
		int enable_recovery; /**< Non-zero to install the panic-recovery middleware.
                          */
		int enable_csrf;     /**< Non-zero to install the CSRF-protection middleware. */
		int enable_auth;     /**< Non-zero to install the token-auth middleware. */
		char* auth_token;    /**< Expected bearer token when @p enable_auth is 1 (NULL
                         = disabled even if enabled). */
	} middleware;		     /**< Built-in middleware toggles. */
	struct {
		char* driver;	/**< AI driver name (e.g., "openai", "claude"). */
		char* model;	/**< AI model identifier (e.g., "gpt-4", "claude-3"). */
		char* api_key;	/**< API key for the AI service. */
		char* base_url; /**< Optional base URL for API requests. */
	} ai;			/**< AI integration settings. */
	struct {
		int enable;   /**< Non-zero to enable cipher functionality. */
		char* driver; /**< Cipher driver name (e.g., "openssl"). */
	} cipher;	      /**< Cipher/cryptography settings. */
} csilk_config_t;

/**
 * @brief Load and parse a YAML configuration file.
 *
 * Reads a YAML file at @p yaml_path and populates @p config.  All string
 * fields in @p config are heap-allocated and must be freed with
 * csilk_config_free.
 *
 * @param  yaml_path  Path to a YAML configuration file.
 * @param[out] config Pointer to a caller-allocated csilk_config_t to populate.
 * @return 0 on success, -1 on failure (parse error or file not found).
 */
int csilk_load_config(const char* yaml_path, csilk_config_t* config);

/**
 * @brief Validate configuration values for semantic correctness.
 *
 * Checks for out-of-range ports, conflicting settings, missing required
 * paths, etc.
 *
 * @param  config     Pointer to the configuration to validate.
 * @param[out] error_msg  Optional pointer to receive a static error string
 *                        (do NOT free).  Unchanged on success.
 * @return 0 if the configuration is valid, -1 if invalid (@p error_msg is set).
 */
int csilk_config_validate(const csilk_config_t* config, const char** error_msg);

/**
 * @brief Free all heap-allocated strings inside a configuration.
 *
 * Does NOT free the csilk_config_t struct itself (only its members).
 * Safe to call on a zero-initialised struct.
 *
 * @param config Pointer to the configuration struct whose fields should be
 * freed.
 */
void csilk_config_free(csilk_config_t* config);

/**
 * @brief Authentication validator callback.
 *
 * Receives the token extracted from the Authorization header and returns
 * non-zero if the token is valid.
 *
 * @param token The bearer token string extracted from the request.
 * @return Non-zero if the token is valid, 0 to reject.
 */
typedef int (*csilk_auth_validator_t)(const char* token);

/**
 * @brief Simple token-based authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, passes it to
 * @p validator, and aborts the chain with 401 if validation fails.
 *
 * @param c         The request context.
 * @param validator Callback that inspects the token and returns 1 for valid,
 *                  0 for invalid.  Called synchronously on the event-loop
 * thread.
 */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator);

/**
 * @brief Serve static files from a local directory.
 *
 * Maps the current request path to a file under @p root_dir.  If the file
 * exists and is readable its contents are sent with the appropriate
 * Content-Type.  If not found the handler chain continues (so a 404 handler
 * can pick it up).
 *
 * @note The URL prefix must be stripped from c->request.path before calling
 *       this function (the low-level API uses the raw path).  Use
 *       csilk_app_static() for automatic prefix handling.
 *
 * @param c        The request context.
 * @param root_dir Absolute or relative path to the directory to serve.
 */
void csilk_static(csilk_ctx_t* c, const char* root_dir);

/**
 * @brief Serve a specific file from the local filesystem.
 *
 * Like csilk_static, this function offloads file I/O to a worker thread and
 * uses zero-copy transmission (sendfile).
 *
 * @param c          The request context.
 * @param file_path  Absolute or relative path to the file.
 */
void csilk_file(csilk_ctx_t* c, const char* file_path);

/**
 * @brief Bind the request body (JSON) to a cJSON object.
 *
 * Parses the raw request body as JSON.  The returned cJSON object is
 * heap-allocated and must be freed by the caller with cJSON_Delete.
 *
 * @param c  The request context.
 * @return A cJSON object parsed from the body, or NULL if the body is
 *         empty or is not valid JSON.
 */
cJSON* csilk_bind_json(csilk_ctx_t* c);

/**
 * @brief Bind request body to cJSON with a descriptive error message.
 *
 * Like csilk_bind_json, but sets @p error to a static string describing why
 * parsing failed.
 *
 * @param  c      The request context.
 * @param[out] error  Pointer to receive a static error string (do NOT free).
 *                    Unchanged on success.
 * @return A cJSON object, or NULL on parse failure (@p error is set).
 */
cJSON* csilk_bind_json_err(csilk_ctx_t* c, const char** error);

/**
 * @brief Get a cookie value by name from the Cookie request header.
 *
 * Parses the Cookie header on first call and caches the result.
 *
 * @param c    The request context.
 * @param name The cookie name.
 * @return The cookie value, or NULL if no cookie with that name exists.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_cookie(csilk_ctx_t* c, const char* name);

/**
 * @brief Set a cookie in the Set-Cookie response header.
 *
 * Appends a Set-Cookie header (using csilk_add_header so multiple cookies
 * are preserved).
 *
 * @param c         The request context.
 * @param name      Cookie name (not URL-encoded — the caller must encode if
 * needed).
 * @param value     Cookie value (not URL-encoded).
 * @param max_age   Lifetime in seconds: >0 = max age, 0 = session cookie, -1 =
 * immediate expiry (delete).
 * @param path      Cookie path scope, or NULL for "/".
 * @param domain    Cookie domain scope, or NULL for current host.
 * @param secure    Non-zero adds the Secure flag (HTTPS only).
 * @param http_only Non-zero adds the HttpOnly flag (not accessible to JS).
 */
void csilk_set_cookie(csilk_ctx_t* c,
		      const char* name,
		      const char* value,
		      int max_age,
		      const char* path,
		      const char* domain,
		      int secure,
		      int http_only);

/**
 * @brief Initialise the session subsystem (call once at startup).
 *
 * Must be called before any request handling.  Sets up the session ID
 * generator and storage backend.
 */
void csilk_session_init(void);

/**
 * @brief Start or resume a session for the current request.
 *
 * If the client sent a session cookie, the existing session is loaded.
 * Otherwise a new session is created and a Set-Cookie header is added.
 *
 * @param c  The request context.
 */
void csilk_session_start(csilk_ctx_t* c);

/**
 * @brief Store a value in the session.
 *
 * @param c     The request context.
 * @param key   Key name (copied internally).
 * @param value Opaque value pointer (ownership remains with caller).
 */
void csilk_session_set(csilk_ctx_t* c, const char* key, void* value);

/**
 * @brief Retrieve a value from the session.
 *
 * @param c   The request context.
 * @param key Key name.
 * @return The value previously stored with csilk_session_set, or NULL if
 *         not found.
 */
void* csilk_session_get(csilk_ctx_t* c, const char* key);

/**
 * @brief Destroy the session and clear the session cookie.
 *
 * Removes all stored session data and sets a Set-Cookie header with an
 * expired session ID to instruct the client to delete it.
 *
 * @param c  The request context.
 */
void csilk_session_destroy(csilk_ctx_t* c);

/** @name Validation flags
 *  Bit flags for use in csilk_valid_rule_t.flags.  Combine with |.
 *  @{ */
#define CSILK_VALID_REQUIRED (1 << 0) /**< Field must be present (non-NULL, non-empty). */
#define CSILK_VALID_INT (1 << 1)      /**< Value must parse as a valid integer. */
#define CSILK_VALID_STRING                                                                         \
	(1 << 2) /**< Value must be a string (always true for form/query values; \
              included for symmetry). */
#define CSILK_VALID_EMAIL                                                                          \
	(1 << 3) /**< Value must match a basic email format (contains '@' and a \
              dot). */
/** @} */

/**
 * @brief A single validation rule for request parameter checking.
 *
 * Rules are collected into a NULL-terminated array and passed to
 * csilk_validate.  Each rule specifies constraints for one field.
 */
typedef struct csilk_valid_rule_s {
	const char* field;  /**< Name of the field to validate. */
	int flags;	    /**< Bitwise OR of CSILK_VALID_* flags.  Set to 0 for no
                        constraints (only min/max apply). */
	int min;	    /**< Minimum allowed length (string fields) or numeric value (int
              fields). */
	int max;	    /**< Maximum allowed length (string fields) or numeric value (int
              fields). */
	const char* source; /**< Location to look for the field: "query", "form",
                         "header", "cookie", or NULL to auto-detect. */
} csilk_valid_rule_t;

/**
 * @brief Validate request parameters against a set of rules.
 *
 * Iterates through the rule array and checks each field for presence,
 * type, and range constraints.  Returns the name of the first field that
 * fails validation.
 *
 * @param c     The request context.
 * @param rules NULL-terminated array of csilk_valid_rule_t.  The array must
 *              end with an entry whose field field is NULL.
 * @return NULL if all rules pass, or a pointer to the failing @p field name
 *         (the returned pointer points into the rule array, not into the
 * context).
 */
const char* csilk_validate(csilk_ctx_t* c, const csilk_valid_rule_t* rules);

/**
 * @brief Send a JSON response (takes ownership of the cJSON object).
 *
 * Serializes @p json to a string, sets the Content-Type header to
 * application/json, and sends the response.  The cJSON object is freed
 * by this function — the caller must not use it afterward.
 *
 * @param c      The request context.
 * @param status HTTP status code.
 * @param json   cJSON object to serialise and send.  Ownership is transferred
 *               to the framework (cJSON_Delete is called internally).
 */
void csilk_json(csilk_ctx_t* c, int status, cJSON* json);

/**
 * @brief Send a JSON-formatted error response.
 *
 * Produces {"error": "<message>"} with the given status code.
 * The message is copied into the request arena.
 *
 * @param c       The request context.
 * @param status  HTTP status code (e.g., 400, 500).
 * @param message Human-readable error description.
 */
void csilk_json_error(csilk_ctx_t* c, int status, const char* message);

/**
 * @brief Parse the JSON request body into a struct registered via reflection.
 *
 * Combines csilk_bind_json_err and csilk_json_unmarshal into one call.
 * The struct at @p ptr must have been registered with CSILK_REGISTER_REFLECT
 * or csilk_reflect_register.
 *
 * @param c         The request context.
 * @param type_name Registered type name string (must match the name used in
 *                  csilk_reflect_register).
 * @param[out] ptr  Pointer to the struct instance to populate.
 * @return 1 on success, 0 on failure (with the response set to a JSON error).
 */
int csilk_bind_reflect(csilk_ctx_t* c, const char* type_name, void* ptr);

/**
 * @brief Serialise a reflected struct as a JSON response.
 *
 * Marshals the struct via csilk_json_marshal and sends the result as a JSON
 * response.  The struct must have been registered with the reflection system.
 *
 * @param c         The request context.
 * @param status    HTTP status code.
 * @param type_name Registered type name string.
 * @param ptr       Pointer to the struct instance to serialise.
 */
void csilk_json_reflect(csilk_ctx_t* c, int status, const char* type_name, const void* ptr);

/** @brief Convenience macro for binding JSON body to a reflected struct.
 *  Wraps csilk_bind_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_bind(c, type, ptr) csilk_bind_reflect(c, #type, ptr)
/** @brief Convenience macro for sending a reflected struct as JSON response.
 *  Wraps csilk_json_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param status HTTP status code.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_json_t(c, status, type, ptr) csilk_json_reflect(c, status, #type, ptr)

/**
 * @brief Get the client's IP address.
 *
 * Checks the X-Forwarded-For / X-Real-IP headers first (if present), then
 * falls back to the socket peer address.
 *
 * @param c  The request context.
 * @return A NUL-terminated IP string, or NULL if the address cannot be
 *         determined.  Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_client_ip(csilk_ctx_t* c);

/**
 * @brief Split a URL into path and query-string components.
 *
 * Internal helper.  The returned @p path and @p query point into the
 * original @p url string (the '?' separator is overwritten with NUL).
 *
 * @param  url    Full URL string (will be modified in-place).
 * @param[out] path  Receives a pointer to the path portion inside @p url.
 * @param[out] query Receives a pointer to the query portion inside @p url,
 *                   or NULL if no query was present.
 */
void csilk_split_url(const char* url, char** path, char** query);

/**
 * @brief Parse a raw query string and populate the query_params map.
 *
 * Internal helper.  Parses key=value pairs separated by '&' and URL-decodes
 * both keys and values.
 *
 * @param c             The request context.
 * @param query_string  Raw query string (the part after '?', may be NULL or
 * empty).
 */
void csilk_parse_query(csilk_ctx_t* c, const char* query_string);

/**
 * @brief Opaque router node type.
 *
 * Nodes form a compressed radix tree (Patricia trie) for efficient path
 * matching.  Each node may hold handlers for one or more HTTP methods.
 */
typedef struct csilk_router_node_s csilk_router_node_t;

/**
 * @brief The main HTTP router.
 *
 * Wraps a radix-tree root node and provides methods to register routes,
 * match incoming requests, and generate OpenAPI specs.
 *
 * @note Not thread-safe for mutation after the server starts.  All routes
 *       must be registered before csilk_server_run.
 */
typedef struct csilk_router_s {
	csilk_router_node_t* root; /**< Root node of the radix (Patricia) trie. */
} csilk_router_t;

/** @brief Route group structure. */
typedef struct csilk_group_s csilk_group_t;

/**
 * @brief Create a new empty router.
 *
 * Allocates and initialises the router structure with a single root node.
 *
 * @return A pointer to the new router (heap-allocated), or NULL on allocation
 *         failure.
 */
csilk_router_t* csilk_router_new(void);

/**
 * @brief Register a route with one or more handlers.
 *
 * The route is inserted into the radix tree.  Dynamic segments (:param) and
 * wildcard segments (*wildcard) are supported in @p path.  The handlers are
 * stored by pointer — the caller must ensure they remain valid for the
 * lifetime of the router.
 *
 * @param r             Router instance.
 * @param method        HTTP method (e.g., "GET", "POST", "DELETE", "*" for
 * any).
 * @param path          URL pattern (e.g., "/users/:id/posts").
 * @param handlers      Array of handler function pointers.
 * @param handler_count Number of elements in @p handlers.
 */
void csilk_router_add(csilk_router_t* r,
		      const char* method,
		      const char* path,
		      csilk_handler_t* handlers,
		      size_t handler_count);

/**
 * @brief Match a raw method+path to handlers (standalone, no context).
 *
 * Useful for testing or when no csilk_ctx_t is available.  The returned
 * array is owned by the router and must NOT be freed.
 *
 * @param r      Router instance.
 * @param method HTTP method string.
 * @param path   Decoded URL path.
 * @return Pointer to the handler array for the matched route, or NULL if
 *         no route matches.
 */
csilk_handler_t* csilk_router_match(csilk_router_t* r, const char* method, const char* path);

/**
 * @brief Match the current request against the router and update the context.
 *
 * On success the matched handlers are stored in the context and path
 * parameters (csilk_get_param) become available.
 *
 * @param r  Router instance.
 * @param c  Request context containing the parsed request.
 * @return 1 if a matching route was found, 0 if no route matches.
 */
int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c);

/**
 * @brief Destroy the router and release all its resources.
 *
 * Frees the radix tree nodes and any associated copy of route metadata
 * (OpenAPI annotations).
 *
 * @param r  Router instance to free.  Must not be NULL.
 */
void csilk_router_free(csilk_router_t* r);

/**
 * @brief Collect all registered routes from the router tree as a cJSON array.
 * @param r The router instance.
 * @return A cJSON array of route objects. Caller must free with cJSON_Delete().
 */
cJSON* csilk_router_collect_routes(csilk_router_t* r);

/**
 * @brief Collect metadata for all registered routes.
 *
 * Traverses the radix tree and returns a cJSON array where each element
 * contains "method", "path", "input_type", "output_type", "summary", and
 * "description" fields.
 *
 * @param r  Router instance.
 * @return A cJSON array (caller must free with cJSON_Delete), or NULL on
 *         allocation failure.
 */
cJSON* csilk_router_collect_routes(csilk_router_t* r);

/**
 * @brief Generate an OpenAPI 3.0 specification JSON from the router.
 *
 * Traverses all registered routes and uses the reflection system to build
 * JSON schemas for request bodies and responses.  Produces a complete
 * OpenAPI document suitable for use with Swagger UI, Redoc, etc.
 *
 * @param router      The router instance.
 * @param title       API title for the OpenAPI info block.
 * @param version     API version for the OpenAPI info block.
 * @param description API description (optional — pass NULL to omit).
 * @return A cJSON object representing the full OpenAPI spec.  Caller must
 *         free with cJSON_Delete.
 */
cJSON* csilk_generate_openapi_json(csilk_router_t* router,
				   const char* title,
				   const char* version,
				   const char* description);

/**
 * @brief Register a route with full OpenAPI/reflection metadata.
 *
 * Extended version of csilk_router_add that also stores metadata for
 * automatic OpenAPI spec generation and request/response binding.
 *
 * @param r             Router instance.
 * @param method        HTTP method string.
 * @param path          URL pattern (e.g., "/users/:id").
 * @param handlers      Array of handler functions.
 * @param handler_count Number of handlers in @p handlers.
 * @param path_pattern  Canonical path pattern string for documentation
 *                      (may differ from the radix-tree path).
 * @param input_type    Registered type name for request-body binding
 *                      (NULL if there is no request body).
 * @param output_type   Registered type name for response serialisation
 *                      (NULL if raw response is used).
 * @param summary       Short summary of the operation (NULL to omit from spec).
 * @param description   Detailed description of the operation (NULL to omit).
 */
void csilk_router_add_extended(csilk_router_t* r,
			       const char* method,
			       const char* path,
			       csilk_handler_t* handlers,
			       size_t handler_count,
			       const char* path_pattern,
			       const char* input_type,
			       const char* output_type,
			       const char* summary,
			       const char* description);

/** @brief Register a route with permission metadata.
 *  @param r             Router instance.
 *  @param method        HTTP method string.
 *  @param path          URL pattern.
 *  @param handlers      Handler function array.
 *  @param handler_count Number of handlers.
 *  @param perm_required Permission identifier (e.g., "read"), or NULL.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or NULL. */
void csilk_router_add_perm(csilk_router_t* r,
			   const char* method,
			   const char* path,
			   csilk_handler_t* handlers,
			   size_t handler_count,
			   const char* perm_required,
			   const char* perm_resource);

/** @brief Register a route with full metadata including permissions.
 *  @param r             Router instance.
 *  @param method        HTTP method string.
 *  @param path          URL pattern.
 *  @param handlers      Handler function array.
 *  @param handler_count Number of handlers.
 *  @param path_pattern  Canonical path pattern for docs.
 *  @param input_type    Registered type name for request-body (NULL if none).
 *  @param output_type   Registered type name for response (NULL if none).
 *  @param summary       Short operation summary (NULL to omit).
 *  @param description   Detailed description (NULL to omit).
 *  @param perm_required Permission identifier (e.g., "read"), or NULL.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or NULL. */
void csilk_router_add_extended_perm(csilk_router_t* r,
				    const char* method,
				    const char* path,
				    csilk_handler_t* handlers,
				    size_t handler_count,
				    const char* path_pattern,
				    const char* input_type,
				    const char* output_type,
				    const char* summary,
				    const char* description,
				    const char* perm_required,
				    const char* perm_resource);

/**
 * @brief Convenience macro to register a route with metadata.
 *
 * Automatically passes @p path as both the URL pattern and the documentation
 * path pattern.  Wraps csilk_router_add_extended.
 */
#define CSILK_ROUTE(                                                                               \
    r, method, path, handlers, handler_count, input_type, output_type, summary, desc)              \
	csilk_router_add_extended(r,                                                               \
				  method,                                                          \
				  path,                                                            \
				  handlers,                                                        \
				  handler_count,                                                   \
				  path,                                                            \
				  input_type,                                                      \
				  output_type,                                                     \
				  summary,                                                         \
				  desc)

/**
 * @brief Serve the OpenAPI JSON specification as the response.
 *
 * Intended to be called from within a handler to expose the API spec.
 *
 * @code
 * void openapi_handler(csilk_ctx_t* c) {
 *     csilk_serve_openapi(c, router, "My API", "1.0.0", "API Description");
 * }
 * @endcode
 *
 * @param c           The request context.
 * @param r           The router instance whose routes will be documented.
 * @param title       API title.
 * @param version     API version.
 * @param description API description (optional, pass NULL to omit).
 */
void csilk_serve_openapi(csilk_ctx_t* c,
			 csilk_router_t* r,
			 const char* title,
			 const char* version,
			 const char* description);

/**
 * @brief Serve the embedded Swagger UI page.
 *
 * The UI loads the OpenAPI spec from /openapi.json (the client fetches it
 * separately).  Register a handler for GET /openapi.json that calls
 * csilk_serve_openapi.
 *
 * @code
 * void docs_handler(csilk_ctx_t* c) {
 *     csilk_serve_swagger_ui(c);
 * }
 * @endcode
 *
 * @param c  The request context.
 */
void csilk_serve_swagger_ui(csilk_ctx_t* c);

/**
 * @brief Create a new route group with a URL prefix.
 *
 * Groups allow sharing a common prefix and middleware set across multiple
 * routes (e.g., "/api/v1").
 *
 * @param router The router to attach the group to.
 * @param prefix URL prefix for all routes in this group (e.g., "/api/v1").
 * @return A new csilk_group_t instance, or NULL on allocation failure.
 */
csilk_group_t* csilk_group_new(csilk_router_t* router, const char* prefix);

/**
 * @brief Create a nested sub-group within an existing group.
 *
 * The sub-group inherits the parent's middleware and its prefix is
 * concatenated.
 *
 * @param parent The parent group.
 * @param prefix Sub-prefix appended to the parent's prefix (e.g., "admin").
 * @return A new sub-group instance, or NULL on allocation failure.
 */
csilk_group_t* csilk_group_group(csilk_group_t* parent, const char* prefix);

/**
 * @brief Add middleware to a group.
 *
 * Middleware is stored in the order it is added and is executed for every
 * route in the group (and any nested sub-groups).
 *
 * @param group   The route group.
 * @param handler Middleware function to prepend to all group routes.
 */
void csilk_group_use(csilk_group_t* group, csilk_handler_t handler);

/**
 * @brief Add a route to the group.
 *
 * The full URL pattern is the group prefix concatenated with @p path.
 * The group's middleware is prepended to the handler.
 *
 * @param group   The route group.
 * @param method  HTTP method.
 * @param path    Path relative to the group prefix (e.g., "/:id").
 * @param handler The route handler function.
 */
void csilk_group_add_route(csilk_group_t* group,
			   const char* method,
			   const char* path,
			   csilk_handler_t handler);

/**
 * @brief Add a route with OpenAPI/reflection metadata to a group.
 *
 * Extended version that also records input/output types and documentation
 * for automatic OpenAPI spec generation.
 *
 * @param group       The route group.
 * @param method      HTTP method.
 * @param path        Path relative to the group prefix.
 * @param handler     The route handler function.
 * @param input_type  Registered type name for request-body binding (NULL if
 * none).
 * @param output_type Registered type name for response serialisation (NULL if
 * none).
 * @param summary     Short operation summary for OpenAPI (NULL to omit).
 * @param description Detailed operation description for OpenAPI (NULL to omit).
 */
void csilk_group_add_route_extended(csilk_group_t* group,
				    const char* method,
				    const char* path,
				    csilk_handler_t handler,
				    const char* input_type,
				    const char* output_type,
				    const char* summary,
				    const char* description);

void csilk_group_add_route_extended_perm(csilk_group_t* group,
					 const char* method,
					 const char* path,
					 csilk_handler_t handler,
					 const char* input_type,
					 const char* output_type,
					 const char* summary,
					 const char* description,
					 const char* perm_required,
					 const char* perm_resource);

/**
 * @brief Add a route with an explicit array of handlers.
 *
 * Useful when you need to attach multiple middleware + the final handler
 * without calling group_use first.
 *
 * @param group    The route group.
 * @param method   HTTP method.
 * @param path     Path relative to the group prefix.
 * @param handlers Array of handler function pointers (middleware first,
 *                 route handler last).  Stored by pointer — must outlive
 *                 the router.
 * @param count    Number of elements in @p handlers.
 */
void csilk_group_add_handlers(csilk_group_t* group,
			      const char* method,
			      const char* path,
			      csilk_handler_t* handlers,
			      size_t count);

/**
 * @brief Destroy a route group and release its resources.
 *
 * Frees the group struct and its prefix string.  Does NOT free the
 * associated router or any handler functions.
 *
 * @param group The group to free.  Must not be NULL.
 */
void csilk_group_free(csilk_group_t* group);

/** @name Group Route Macros
 * Convenience macros for adding routes to groups.
 * @{ */
/** @brief Register a GET route on the group. */
#define csilk_GET(group, path, handler) csilk_group_add_route(group, "GET", path, handler)
/** @brief Register a POST route on the group. */
#define csilk_POST(group, path, handler) csilk_group_add_route(group, "POST", path, handler)
/** @brief Register a PUT route on the group. */
#define csilk_PUT(group, path, handler) csilk_group_add_route(group, "PUT", path, handler)
/** @brief Register a DELETE route on the group. */
#define csilk_DELETE(group, path, handler) csilk_group_add_route(group, "DELETE", path, handler)
/** @brief Register a PATCH route on the group. */
#define csilk_PATCH(group, path, handler) csilk_group_add_route(group, "PATCH", path, handler)
/** @brief Register an OPTIONS route on the group. */
#define csilk_OPTIONS(group, path, handler) csilk_group_add_route(group, "OPTIONS", path, handler)
/** @brief Register a HEAD route on the group. */
#define csilk_HEAD(group, path, handler) csilk_group_add_route(group, "HEAD", path, handler)
/** @} */

/**
 * @brief Perform the WebSocket upgrade handshake (HTTP Upgrade request).
 *
 * Validates the Upgrade, Connection, Sec-WebSocket-Key, and version headers,
 * computes the Sec-WebSocket-Accept response, and sends a 101 Switching
 * Protocols response.  After success, csilk_is_websocket returns 1 and the
 * connection can send/receive frames.
 *
 * @param c  The request context.
 */
void csilk_ws_handshake(csilk_ctx_t* c);

/**
 * @brief Send a WebSocket data frame.
 *
 * Encodes and sends a single WebSocket frame per RFC 6455.  Masks the
 * payload if required (client-to-server masking).
 *
 * @param c       The request context.
 * @param payload Raw data to send.
 * @param len     Byte length of @p payload.
 * @param opcode  WebSocket opcode: 0x1 for text, 0x2 for binary, 0x9 for ping.
 */
void csilk_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

/**
 * @brief Send a WebSocket close frame.
 *
 * Initiates the close handshake per RFC 6455 §5.5.1.  After sending, the
 * server waits for the client's close frame before fully closing the TCP
 * connection.
 *
 * @param c           The request context.
 * @param status_code Close status code (e.g., 1000 for normal closure,
 *                    0 to omit the status code from the frame).
 * @param reason      Optional human-readable reason string (may be NULL).
 */
void csilk_ws_close(csilk_ctx_t* c, uint16_t status_code, const char* reason);

/* --- Streaming Response (Chunked Transfer Encoding) --- */

/**
 * @brief Write a chunk to the response stream (chunked transfer encoding).
 *
 * The first call sends the HTTP response headers with
 * Transfer-Encoding: chunked.  Subsequent calls append chunked frames.
 * The handler MUST set async mode (csilk_set_async(c, 1)) before calling
 * this and MUST NOT use csilk_string, csilk_json, etc.
 *
 * @param c    The request context.
 * @param data Raw data for the chunk.
 * @param len  Byte length of @p data.
 */
void csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len);

/**
 * @brief Finalise a chunked streaming response.
 *
 * Sends the terminal (zero-length) chunk and any trailers.  The connection
 * is then kept alive or closed according to the HTTP keep-alive header.
 *
 * @param c  The request context.
 */
void csilk_response_end(csilk_ctx_t* c);

/* --- Server-Sent Events (SSE) --- */

/**
 * @brief Initialise a Server-Sent Events connection.
 *
 * Sends the HTTP 200 response with Content-Type: text/event-stream and
 * disables request buffering.  Must be called at the start of an SSE
 * handler before any csilk_sse_send calls.
 *
 * @param c  The request context.
 */
void csilk_sse_init(csilk_ctx_t* c);

/**
 * @brief Send an SSE event (or comment) to the client.
 *
 * Formats and flushes one SSE message.  If @p event is NULL and @p data is
 * non-NULL, a default "message" event is sent.  If @p data is NULL, a
 * comment line (starting with ":") is written.
 *
 * @param c     The request context.
 * @param event Optional event type string (e.g., "update"), or NULL.
 * @param data  Event data string, or NULL to send a comment line.
 */
void csilk_sse_send(csilk_ctx_t* c, const char* event, const char* data);

/**
 * @brief Close the SSE connection.
 *
 * Sends any remaining buffered data and closes the TCP connection.
 *
 * @param c  The request context.
 */
void csilk_sse_close(csilk_ctx_t* c);

/* --- JWT Authentication Middleware --- */

/**
 * @brief Generate a signed JWT token (HS256).
 *
 * Creates a three-part JWT (header.payload.signature) using HMAC-SHA256.
 * The @p payload is used as-is for the claims.
 *
 * @param c       Request context (for crypto-driver access).
 * @param payload cJSON object containing JWT claims (e.g., {"sub":"123"}).
 *                Not modified; ownership stays with caller.
 * @param secret  Secret key string for HMAC signing.
 * @return A heap-allocated JWT string (caller must free), or NULL on failure.
 */
char* csilk_jwt_generate(csilk_ctx_t* c, cJSON* payload, const char* secret);

/**
 * @brief Verify a JWT token and extract its payload.
 *
 * Validates the signature (HMAC-SHA256), checks the "exp" claim if present,
 * and returns the parsed payload.
 *
 * @param c      Request context (for crypto-driver access).
 * @param token  The JWT string to verify.
 * @param secret Secret key for HMAC verification.
 * @return A heap-allocated cJSON object with the payload claims, or NULL if
 *         the token is invalid or expired.  Caller must free with cJSON_Delete.
 */
cJSON* csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret);

/**
 * @brief JWT authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, verifies it,
 * and stores the decoded payload in the context under the key "jwt_payload".
 * If the token is missing or invalid, responds with 401 Unauthorized and
 * aborts the chain.
 *
 * @param c      The request context.
 * @param secret Secret key for token verification.
 */
void csilk_jwt_middleware(csilk_ctx_t* c, const char* secret);

/* --- Gzip Compression Middleware --- */

/**
 * @brief Gzip response compression middleware.
 *
 * If the client advertises gzip/deflate support (Accept-Encoding header),
 * this middleware compresses the response body transparently.  Must be
 * registered as a group-level or server-level middleware that wraps the
 * handler chain.
 *
 * @param c  The request context.
 */
void csilk_gzip_middleware(csilk_ctx_t* c);

/* --- Multipart Form Data --- */

/**
 * @brief A single part parsed from a multipart/form-data request body.
 *
 * Contains the field name, optional filename (for file uploads), content
 * type, and the binary data.  Strings are NUL-terminated fixed-size buffers;
 * data longer than the buffer is truncated.
 */
typedef struct csilk_multipart_part_s {
	char name[128];	       /**< Form field name (NUL-terminated).  Truncated to 127
                         chars. */
	char filename[256];    /**< Original filename for file uploads (empty string if
                         not a file).  Truncated to 255 chars. */
	char content_type[64]; /**< Content-Type of the part (e.g., "image/png").
                            Truncated to 63 chars. */
	uint8_t* data;	       /**< Pointer to the part's binary data.  Valid until
                            csilk_ctx_cleanup. */
	size_t data_len;       /**< Byte length of @p data. */
	csilk_ctx_t* ctx;      /**< Owning request context (for memory allocation). */
} csilk_multipart_part_t;

/**
 * @brief Callback invoked for each part during multipart parsing.
 *
 * @param part The parsed part.  The data pointer is valid only during the
 *             callback invocation — do not store the pointer for later use
 *             (copy the data if needed).
 */
typedef void (*csilk_multipart_handler_t)(csilk_multipart_part_t* part);

/**
 * @brief Parse a multipart/form-data request body.
 *
 * Iterates over all parts in the request body (using the Content-Type
 * boundary) and calls @p handler for each.  Files and form fields are
 * treated uniformly — check the filename field to distinguish them.
 *
 * @param c       The request context.
 * @param handler Callback invoked once per part.
 */
void csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler);

/** @brief Main Server structure. */
typedef struct csilk_server_s csilk_server_t;

/**
 * @brief Get the server instance associated with the current context.
 * @param c The request context.
 * @return Server handle, or NULL on error.
 */
csilk_server_t* csilk_ctx_get_server(csilk_ctx_t* c);

/* --- Hook System --- */

/**
 * @brief Lifecycle hook types for the server and individual requests.
 *
 * Hooks allow users to inject custom logic at well-defined points in the
 * server and request lifecycle without modifying the framework code.
 */
typedef enum {
	CSILK_HOOK_SERVER_START,  /**< Invoked just before the event loop starts
                               (server-level). */
	CSILK_HOOK_SERVER_STOP,	  /**< Invoked when the server is shutting down
                               (server-level). */
	CSILK_HOOK_CONN_OPEN,	  /**< Invoked when a new TCP connection is accepted
                               (context-level). */
	CSILK_HOOK_CONN_CLOSE,	  /**< Invoked when a TCP connection is closed
                               (context-level). */
	CSILK_HOOK_REQUEST_BEGIN, /**< Invoked when the full HTTP request has been
                               parsed (context-level). */
	CSILK_HOOK_REQUEST_END,	  /**< Invoked after the response has been sent
                               (context-level). */
	CSILK_HOOK_COUNT	  /**< Sentinel — total number of hook types. Not a valid hook
                      type. */
} csilk_hook_type_t;

/**
 * @brief Callback signature for server-level hooks.
 *
 * @param s The server instance.
 */
typedef void (*csilk_server_hook_handler_t)(csilk_server_t* s);

/**
 * @brief Callback signature for request/connection-level hooks.
 *
 * @param c The request context.
 */
typedef void (*csilk_ctx_hook_handler_t)(csilk_ctx_t* c);

/**
 * @brief Register a lifecycle hook callback.
 *
 * The @p handler is cast to the appropriate type internally based on
 * @p type.  Multiple handlers may be registered for the same hook type.
 *
 * @param s       The server instance.
 * @param type    The hook type (see csilk_hook_type_t).
 * @param handler Pointer to the callback function.  Must match the expected
 *                signature for @p type (csilk_server_hook_handler_t for
 *                SERVER_*, csilk_ctx_hook_handler_t for others).
 */
void csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler);

/* --- Crypto Driver Interface --- */

/**
 * @brief Pluggable cryptographic primitive driver.
 *
 * Allows users to replace the default software implementations of SHA256,
 * HMAC-SHA256, and UUID generation (e.g., with hardware-accelerated or
 * FIPS-compliant versions).  All function pointers must be non-NULL.
 */
typedef struct {
	/** @brief Compute the SHA-256 hash of a buffer.
   *  @param data  Input data.
   *  @param len   Input length.
   *  @param[out] out  32-byte hash output. */
	void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
	/** @brief Compute HMAC-SHA256.
   *  @param key       HMAC key.
   *  @param key_len   Key length.
   *  @param data      Input data.
   *  @param data_len  Input length.
   *  @param[out] out  32-byte HMAC output. */
	void (*hmac_sha256)(const uint8_t* key,
			    size_t key_len,
			    const uint8_t* data,
			    size_t data_len,
			    uint8_t out[32]);
	/** @brief Generate a random version-4 UUID string.
   *  @param[out] buf  Output buffer of at least 37 bytes.  Populated with a
   *                   NUL-terminated UUID string. */
	void (*generate_uuid)(char buf[37]);
} csilk_crypto_driver_t;

/**
 * @brief Set the global crypto driver for the server.
 *
 * Replaces the default software crypto routines with a user-provided
 * implementation.  Pass NULL to restore the built-in defaults.
 *
 * @param server The server instance.
 * @param driver Pointer to a csilk_crypto_driver_t, or NULL for defaults.
 *               The driver struct must remain valid for the server's lifetime.
 */
void csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver);

/**
 * @brief Set the cipher driver for symmetric/asymmetric encryption.
 *
 * Replaces the default OpenSSL-based AES-256-GCM / RSA-OAEP / RSA-PSS
 * implementations with a user-provided driver.  Pass NULL to restore the
 * built-in defaults.
 *
 * @param server The server instance.
 * @param driver Pointer to a csilk_cipher_driver_t, or NULL for defaults.
 *               The driver struct must remain valid for the server's lifetime.
 */
void csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver);

/**
 * @brief Set the storage driver for the context.
 * @param c       The request context.
 * @param driver  The storage driver vtable.
 */
void csilk_ctx_set_storage_driver(csilk_ctx_t* c, csilk_storage_driver_t* driver);

/**
 * @brief Set the crypto driver for the context.
 * @param c       The request context.
 * @param driver  The crypto driver vtable.
 */
void csilk_ctx_set_crypto_driver(csilk_ctx_t* c, csilk_crypto_driver_t* driver);

/**
 * @brief Set the cipher driver for the context.
 * @param c       The request context.
 * @param driver  The cipher driver vtable.
 */
void csilk_ctx_set_cipher_driver(csilk_ctx_t* c, csilk_cipher_driver_t* driver);

/**
 * @brief Create a new server instance.
 *
 * Allocates and initialises a server bound to the given router.  The server
 * takes ownership of the router and frees it in csilk_server_free.
 *
 * @param router The router to use for request dispatch.  Must not be NULL.
 * @return A new server instance, or NULL on allocation failure.
 */
csilk_server_t* csilk_server_new(csilk_router_t* router);

/**
 * @brief Register global middleware.
 *
 * Middleware is executed for every request in the order it was added.  The
 * handler array has a fixed maximum size (typically 64).
 *
 * @param server  Server instance.
 * @param handler Middleware function.
 * @return 0 on success, -1 if the internal handler array is full.
 */
int csilk_server_use(csilk_server_t* server, csilk_handler_t handler);

/**
 * @brief Set a custom handler for 404 (route-not-found) responses.
 *
 * Replaces the default 404 behaviour.  The handler is invoked with the
 * request context (status 404 is NOT pre-set — the handler may set its own).
 * Pass NULL to restore the default 404 handler.
 *
 * @param server  Server instance.
 * @param handler Handler function, or NULL for default.
 */
void csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler);

/**
 * @brief Enable single-page application (SPA) fallback mode.
 *
 * Unmatched GET requests serve index.html from @p doc_root instead of
 * returning 404.  Overrides any custom 404 handler.  Useful for serving
 * React/Vue/Angular SPAs where the router handles URLs client-side.
 *
 * @param server   Server instance.
 * @param doc_root Directory containing index.html.  The path is copied
 *                 internally.
 */
void csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root);

/**
 * @brief Destroy the server and release all resources.
 *
 * Stops the server if running, closes all connections, and frees the
 * router, hooks, and internal structures.
 *
 * @param server Server instance to free.
 */
void csilk_server_free(csilk_server_t* server);

/**
 * @brief Request a graceful server shutdown.
 *
 * Signals the event loop to stop after all active requests complete.
 * New connections are refused.
 *
 * @param server Server instance.
 */
void csilk_server_stop(csilk_server_t* server);
void csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn);

/**
 * @brief Apply server configuration options.
 *
 * Copies values from @p config into the server's internal state.  Should
 * be called before csilk_server_run.  The config struct may be stack-allocated.
 *
 * @param server Server instance.
 * @param config Pointer to the configuration to apply.
 */
void csilk_server_set_config(csilk_server_t* server, const csilk_server_config_t* config);

/**
 * @brief Set the maximum number of concurrent connections and return the
 * previous limit.
 *
 * @param server Server instance.
 * @param max    New limit (0 = unlimited).
 * @return The previous maximum connections value.
 */
int csilk_server_set_max_connections(csilk_server_t* server, int max);

/**
 * @brief Replace the context key-value storage driver.
 *
 * @param server Server instance.
 * @param driver Pointer to the new driver, or NULL to restore the default
 *               in-memory arena-backed driver.  The driver struct must remain
 *               valid for the server's lifetime.
 */
void csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver);

/**
 * @brief Start the server and enter the libuv event loop.
 *
 * This call blocks until the server is stopped (csilk_server_stop) or a
 * fatal error occurs.
 *
 * @param server Server instance.
 * @param port   TCP port to listen on.
 * @return 0 on normal shutdown, -1 on initialisation failure.
 */
int csilk_server_run(csilk_server_t* server, int port);

/* --- Database Interface --- */

/**
 * @brief Initialise the database subsystem.
 *
 * Registers built-in drivers (SQLite3, etc.).  Must be called once before
 * any csilk_db_pool_new call.  Safe to call multiple times.
 */
void csilk_db_init(void);

/**
 * @brief Create a new database connection pool.
 *
 * The pool maintains a single connection (or opens a new one on demand).
 * All database operations go through the pool, which provides mutex-based
 * thread safety.
 *
 * @param driver_name  Driver identifier (e.g., "sqlite").  Must have been
 *                     registered via csilk_db_register_driver or the built-in
 *                     init.
 * @param dsn          Data source name (driver-specific, e.g., "file:test.db").
 * @return A new pool instance, or NULL if the driver is unknown or connection
 *         fails.
 */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/**
 * @brief Free a database pool and disconnect.
 *
 * Closes the underlying connection and frees the pool struct.
 *
 * @param pool  The pool to free.  Must not be NULL.
 */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/**
 * @brief Execute a SELECT query and return the result as a JSON array.
 *
 * Each row becomes a JSON object keyed by column name.
 *
 * @param pool  Connection pool.
 * @param sql   SQL SELECT statement.
 * @return A cJSON array of row objects (caller must free with cJSON_Delete),
 *         or NULL on failure.
 */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a statement that returns no result rows.
 *
 * Suitable for INSERT, UPDATE, DELETE, CREATE TABLE, etc.
 *
 * @param pool  Connection pool.
 * @param sql   SQL statement.
 * @return 0 on success, -1 on failure.
 */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders.
 *
 * Each ? in @p sql is replaced with the corresponding value from @p params
 * (The driver handles escaping internally).  The result is returned as a
 * JSON array.
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params NULL-terminated array of string values for the placeholders.
 *               The array must end with a NULL sentinel.
 * @return A cJSON array (caller must free), or NULL on failure.
 */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params);

/**
 * @brief Prometheus metrics middleware.
 *
 * Tracks request-level metrics: QPS, latency distribution histogram, and
 * HTTP status code counters.  Should be added early in the middleware chain.
 *
 * @param c   The request context.
 * @param arg Optional config string (currently unused, pass NULL).
 */
void csilk_metrics_middleware(csilk_ctx_t* c, const char* arg);

/**
 * @brief Prometheus /metrics endpoint handler.
 *
 * Exposes collected metrics in the standard Prometheus text exposition format
 * (content-type: text/plain; version=0.0.4).
 *
 * @param c  The request context.
 */
void csilk_metrics_handler(csilk_ctx_t* c);
uint64_t csilk_metrics_get_total_requests(void);
uint64_t csilk_metrics_get_total_duration(void);

/**
 * @brief Opaque Message Queue (event bus) instance.
 *
 * Provides an in-process pub/sub system built on libuv async handles.
 * Thread-safe publishing allows worker threads to send messages to the
 * main event loop.  Supports middleware chains, persistence via WAL, and
 * background offloading.
 */
typedef struct csilk_mq_s csilk_mq_t;

/** @brief Get the internal MQ instance from the context.
 *  @param c The request context.
 *  @return Pointer to csilk_mq_t, or NULL if not available. */
csilk_mq_t* csilk_ctx_get_mq(csilk_ctx_t* c);

/**
 * @brief Get the Message Queue instance attached to a server.
 *
 * The MQ is created lazily on first access.
 *
 * @param server  Pointer to the server instance.
 * @return Pointer to the server's MQ, or NULL if the server is not yet
 *         initialised.
 */
csilk_mq_t* csilk_server_get_mq(csilk_server_t* server);

/** @brief Get the router instance attached to a server.
 *  @param server The server instance.
 *  @return Pointer to csilk_router_t. */
csilk_router_t* csilk_server_get_router(csilk_server_t* server);

/**
 * @brief Opaque Message Queue context.
 *
 * Created per-message and passed to middleware and subscriber handlers.
 * Provides access to the topic, payload, and chain-control functions.
 * Valid only during the handler invocation — do not store the pointer.
 */
typedef struct csilk_mq_ctx_s csilk_mq_ctx_t;

/**
 * @brief MQ handler signature for middleware and subscribers.
 *
 * @param ctx  MQ context providing topic, payload, and chain control.
 */
typedef void (*csilk_mq_handler_t)(csilk_mq_ctx_t* ctx);

/**
 * @brief Pass control to the next middleware or subscriber in the MQ chain.
 *
 * Must be called exactly once (or zero times if csilk_mq_abort is used)
 * for the chain to advance.
 *
 * @param ctx  The MQ context.
 */
void csilk_mq_next(csilk_mq_ctx_t* ctx);

/**
 * @brief Abort the MQ middleware/subscriber chain.
 *
 * No further handlers execute for the current message.
 *
 * @param ctx  The MQ context.
 */
void csilk_mq_abort(csilk_mq_ctx_t* ctx);

/**
 * @brief Signature for a background MQ worker function.
 *
 * @param topic   The topic string (valid only during the call).
 * @param payload Opaque data pointer.
 * @param len     Byte length of @p payload.
 */
typedef void (*csilk_mq_worker_t)(const char* topic, const void* payload, size_t len);

/**
 * @brief Offload message processing to a background thread.
 *
 * Hands off the current message to libuv's thread pool for processing.
 * csilk_mq_next is called internally so the chain continues immediately.
 * The @p worker runs on a separate thread — it must be thread-safe and must
 * NOT call back into the MQ or context APIs.
 *
 * @param ctx    The MQ context.
 * @param worker Background worker function that receives the topic and
 *               a copy of the payload.
 */
void csilk_mq_offload(csilk_mq_ctx_t* ctx, csilk_mq_worker_t worker);

/**
 * @brief Get the topic of the current message.
 *
 * @param ctx  The MQ context.
 * @return The topic string.  Valid only for the duration of the handler call.
 */
const char* csilk_mq_get_topic(csilk_mq_ctx_t* ctx);

/**
 * @brief Get the payload of the current message.
 *
 * @param ctx      The MQ context.
 * @param[out] len Optional pointer to receive the payload byte length (may be
 * NULL).
 * @return Pointer to the message payload.  Valid only for the duration of the
 *         handler call.  The pointer must NOT be freed.
 */
const void* csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len);

/**
 * @brief Register MQ middleware for a topic.
 *
 * Middleware runs before subscribers.  Pass NULL as @p topic to register
 * global middleware that intercepts all messages.
 *
 * @param mq         The MQ instance.
 * @param topic      Topic name to intercept, or NULL for global middleware.
 * @param middleware Handler function.  Must not be NULL.
 */
void csilk_mq_use(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t middleware);

/**
 * @brief Register a subscriber for a topic.
 *
 * Subscribers run after all applicable middleware (global + topic-specific)
 * has completed.
 *
 * @param mq         The MQ instance.
 * @param topic      Topic name to subscribe to.
 * @param subscriber Handler function.  Must not be NULL.
 */
void csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber);

/**
 * @brief Publish a message to a topic.
 *
 * The payload is **copied** internally so the caller can reuse the buffer
 * immediately.  The message is enqueued and processed asynchronously on the
 * main event loop via a libuv async handle, making this function thread-safe.
 *
 * @param mq      The MQ instance.
 * @param topic   Target topic name.
 * @param payload Pointer to the data to publish (copied internally).
 * @param len     Byte length of @p payload.
 * @return 0 on success, non-zero errno-compatible code on failure (typically
 *         ENOMEM).
 */
int csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/**
 * @brief Message Queue statistics.
 */
typedef struct {
	uint64_t published_total; /**< Total messages published since startup. */
	uint64_t delivered_total; /**< Total messages delivered to subscribers. */
	uint64_t failed_total;	  /**< Total messages that failed processing. */
	uint32_t queue_depth;	  /**< Number of messages currently in memory queue. */
	uint32_t topic_count;	  /**< Number of registered topics. */
} csilk_mq_stats_t;

/**
 * @brief Get current MQ statistics.
 * @param mq    The MQ instance.
 * @param stats [out] Pointer to stats struct to populate.
 */
void csilk_mq_get_stats(csilk_mq_t* mq, csilk_mq_stats_t* stats);

/**
 * @brief Convert MQ statistics to a JSON string.
 * @param stats Pointer to stats struct.
 * @return Heap-allocated JSON string (must be freed).
 */
char* csilk_mq_stats_to_json(const csilk_mq_stats_t* stats);

/**
 * @brief Register a WebSocket monitor for real-time MQ events.
 * @param mq The MQ instance.
 * @param c  Framework context (WebSocket connection).
 */
void csilk_mq_register_monitor(csilk_mq_t* mq, csilk_ctx_t* c);

/**
 * @brief Enable Write-Ahead Log (WAL) persistence for the MQ.
 *
 * When enabled, every published message is appended to @p wal_path before
 * being processed.  The WAL can be replayed on restart to recover messages.
 *
 * @param mq       The MQ instance.
 * @param wal_path File path for the WAL (e.g., "mq.wal").  The string is
 *                 copied internally.
 * @return 0 on success, non-zero on file-open failure.
 */
int csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path);

#endif /* CSILK_H */
