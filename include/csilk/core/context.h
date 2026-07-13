#pragma once
#include "csilk/core/sys_io.h"
/**
 * @file context.h
 * @brief Per-request context accessors and mutators for the csilk framework.
 *
 * All functions operate on the opaque csilk_ctx_t handle, providing
 * read/write access to the request, response, path parameters, arena,
 * storage, and connection state.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#include "csilk/core/types.h"
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"

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
 * Only valid after the full body has been parsed.  Returns nullptr for methods
 * that have no body (GET, HEAD, etc.) or when the body is empty.
 *
 * @param c        The request context.
 * @param[out] out_len  Optional pointer to receive the body length in bytes.
 *                      May be nullptr if the caller does not need the length.
 * @return Pointer to the raw body data (not NUL-terminated), or nullptr if no
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
 * @brief Enable or disable WebSocket mode.
 *
 * @param c             The request context.
 * @param is_websocket  1 to enable, 0 to disable.
 */
void csilk_ctx_set_websocket(csilk_ctx_t* c, int is_websocket);

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
void csilk_ctx_set_sse(csilk_ctx_t* c, int is_sse);

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
 * @brief Get the request headers map.
 *
 * @param c  The request context.
 * @return Pointer to the opaque header map.
 */
csilk_header_map_t* csilk_get_headers(csilk_ctx_t* c);

/**
 * @brief Get a request header value by name (case-insensitive).
 *
 * @param c   The request context.
 * @param key The header field name (e.g., "Content-Type").
 * @return The header value string, or nullptr if the header is not present.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_header(csilk_ctx_t* c, const char* key);

/**
 * @brief Get a response header value by name (case-insensitive).
 *
 * @param c   The request context.
 * @param key The header field name.
 * @return The header value string, or nullptr if the header has not been set.
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
 * @return The parameter value, or nullptr if not present.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_query(csilk_ctx_t* c, const char* key);

/**
 * @brief Get a URL path parameter by key.
 *
 * Parameters are extracted from the route pattern by the router.  For a route
 * `/users/:id`, csilk_get_param(c, "id") returns the actual value.
 *
 * @param c   The request context.
 * @param key The parameter name as defined in the route pattern (e.g., "id").
 * @return The decoded parameter value, or nullptr if @p key is not a known
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
 * @return The parameter name string, or nullptr if index is out of bounds.
 */
const char* csilk_get_param_key(csilk_ctx_t* c, int index);

/**
 * @brief Get the value of a path parameter by its index.
 *
 * @param c      The request context.
 * @param index  Index of the parameter (0..count-1).
 * @return The parameter value string, or nullptr if index is out of bounds.
 */
const char* csilk_get_param_value(csilk_ctx_t* c, int index);

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
 * @brief Set the request unique identifier.
 *
 * @param c   The request context.
 * @param id  The new request ID string. It is copied into the context.
 */
void csilk_set_request_id(csilk_ctx_t* c, const char* id);

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
 * @brief Set the current response status code.
 *
 * @param c  The request context.
 * @param status The HTTP status code to set.
 */
void csilk_set_status(csilk_ctx_t* c, int status);

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
void csilk_ctx_set_async(csilk_ctx_t* c, int is_async);

/**
 * @brief Check whether asynchronous response mode is enabled.
 *
 * @param c  The request context.
 * @return 1 if async mode is active, 0 if the framework owns response flushing.
 */
int csilk_is_async(csilk_ctx_t* c);

/**
 * @brief Dispatch a callback to be executed on the event loop thread owning this context.
 * 
 * This allows background threads (e.g., Python asyncio threads) to safely execute
 * code on the main I/O event loop (libuv or io_uring) to send responses.
 * 
 * @param c The request context.
 * @param cb The callback function to execute.
 * @param arg Opaque argument passed to the callback.
 */
void csilk_dispatch(csilk_ctx_t* c, void (*cb)(void* arg), void* arg);

/**
 * @brief Get the index of the currently executing handler in the chain.
 *
 * @param c  The request context.
 * @return Index (0-based) or -1 if the chain hasn't started.
 */
int csilk_get_handler_index(csilk_ctx_t* c);

/**
 * @brief Get the I/O work request associated with the context.
 *
 * Use this to offload long-running operations to the thread pool while
 * maintaining context state.
 *
 * @param c  The request context.
 * @return Pointer to the context's internal csilk_io_work_t.
 */
csilk_io_work_t* csilk_get_work_req(csilk_ctx_t* c);

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
 * @return The route path pattern, or nullptr if no route was matched.
 */
const char* csilk_ctx_get_handler_path(csilk_ctx_t* c);

/**
 * @brief Get the permission string required by the matched handler.
 *
 * @param c  The request context.
 * @return The permission identifier, or nullptr if none is required.
 */
const char* csilk_ctx_get_handler_perm_required(csilk_ctx_t* c);

/**
 * @brief Get the resource pattern for the matched handler's permission check.
 *
 * @param c  The request context.
 * @return The resource pattern, or nullptr.
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
 * @brief Get the server instance associated with the current context.
 * @param c The request context.
 * @return Server handle, or nullptr on error.
 */
csilk_server_t* csilk_ctx_get_server(csilk_ctx_t* c);

/** @brief Get the internal MQ instance from the context.
 *  @param c The request context.
 *  @return Pointer to csilk_mq_t, or nullptr if not available. */
csilk_mq_t* csilk_ctx_get_mq(csilk_ctx_t* c);

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
 * @param value Opaque pointer to store.  May be nullptr (which will be returned
 *              by csilk_get, so storing nullptr is indistinguishable from "not
 *              set" — avoid it).
 */
void csilk_set(csilk_ctx_t* c, const char* key, void* value);

/**
 * @brief Retrieve an opaque value from the request context.
 *
 * @param c   The request context.
 * @param key NUL-terminated key name.
 * @return The value pointer previously stored with csilk_set, or nullptr if
 *         @p key was never set (or was explicitly set to nullptr — see the
 *         note on csilk_set).
 */
void* csilk_get(csilk_ctx_t* c, const char* key);

/**
 * @brief Store a string value with an optional TTL (requires a compatible storage driver).
 * @param c        The request context.
 * @param key      Storage key.
 * @param value    String value.
 * @param ttl_sec  Time-to-live in seconds (0 = no expiry).
 * @return 0 on success, non-zero on failure.
 */
int csilk_set_string(csilk_ctx_t* c, const char* key, const char* value, int ttl_sec);

/**
 * @brief Retrieve a string value by key (requires a compatible storage driver).
 * @param c    The request context.
 * @param key  Storage key.
 * @return Heap-allocated string value (caller must free), or nullptr if not found.
 */
char* csilk_get_string(csilk_ctx_t* c, const char* key);

/**
 * @brief Increment a numeric value by 1 with an optional TTL (requires a compatible storage driver).
 * @param c        The request context.
 * @param key      Storage key.
 * @param ttl_sec  Time-to-live in seconds (set only if newly created; 0 = no expiry).
 * @return The new value after incrementing, or -1 on error.
 */
long long csilk_incr(csilk_ctx_t* c, const char* key, int ttl_sec);

/**
 * @brief Bind the request body (JSON) to a cJSON object.
 *
 * Parses the raw request body as JSON.  The returned cJSON object is
 * heap-allocated and must be freed by the caller with cJSON_Delete.
 *
 * @param c  The request context.
 * @return A cJSON object parsed from the body, or nullptr if the body is
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
 * @return A cJSON object, or nullptr on parse failure (@p error is set).
 */
cJSON* csilk_bind_json_err(csilk_ctx_t* c, const char** error);

/**
 * @brief Get a cookie value by name from the Cookie request header.
 *
 * Parses the Cookie header on first call and caches the result.
 *
 * @param c    The request context.
 * @param name The cookie name.
 * @return The cookie value, or nullptr if no cookie with that name exists.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_cookie(csilk_ctx_t* c, const char* name);

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

/** @brief Convenience macro for binding JSON body to a reflected struct.
 *  Wraps csilk_bind_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_bind(c, type, ptr) csilk_bind_reflect(c, #type, ptr)

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
 * @return The field value, or nullptr if not found.
 *         Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_form_field(csilk_ctx_t* c, const char* key);

/**
 * @brief Parse a raw query string and populate the query_params map.
 *
 * Internal helper.  Parses key=value pairs separated by '&' and URL-decodes
 * both keys and values.
 *
 * @param c             The request context.
 * @param query_string  Raw query string (the part after '?', may be nullptr or
 * empty).
 */
void csilk_parse_query(csilk_ctx_t* c, const char* query_string);

/**
 * @brief Iterate over all request headers.
 *
 * @param c   The request context.
 * @param cb  Callback function invoked for each header.
 * @param arg User-provided closure argument.
 */
void csilk_for_each_header(csilk_ctx_t* c, csilk_header_cb cb, void* arg);

/**
 * @brief Iterate over all query parameters.
 *
 * @param c   The request context.
 * @param cb  Callback function invoked for each query parameter.
 * @param arg User-provided closure argument.
 */
void csilk_for_each_query(csilk_ctx_t* c, csilk_header_cb cb, void* arg);

/**
 * @brief Iterate over all form-urlencoded parameters.
 *
 * Only meaningful after csilk_parse_form_urlencoded() has been called.
 *
 * @param c   The request context.
 * @param cb  Callback function invoked for each form parameter.
 * @param arg User-provided closure argument.
 */
void csilk_for_each_form_field(csilk_ctx_t* c, csilk_header_cb cb, void* arg);

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
 * @return The callback function pointer, or nullptr if none is set.
 */
void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t*   c,
                                                const uint8_t* payload,
                                                size_t         len,
                                                int            opcode);

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
 * @brief Register a deferred cleanup callback for the request context.
 *
 * The callback @p fn is invoked with @p arg when the context is cleaned
 * up — including during panic recovery (csilk_panic / longjmp).  This
 * protects heap allocations, file descriptors, and mutex locks held by
 * handlers from leaking when longjmp skips stack unwinding.
 *
 * Items are arena-allocated and freed automatically on arena reset.
 * Callbacks are invoked in LIFO order (most recently registered first).
 *
 * Example:
 * @code
 *   char* buf = malloc(1024);
 *   csilk_ctx_defer(c, free, buf);
 * @endcode
 *
 * @param c   The request context.
 * @param fn  Cleanup function (e.g., free, close, csilk_mutex_unlock).
 * @param arg Argument passed to @p fn.
 * @return 0 on success, -1 on allocation failure.
 */
int csilk_ctx_defer(csilk_ctx_t* c, void (*fn)(void*), void* arg);

/**
 * @brief Execute all deferred cleanup callbacks and clear the list.
 *
 * Walks the defer linked list in LIFO order and calls each registered
 * cleanup function with its argument. After execution the list is cleared
 * (subsequent calls are no-ops).  Called automatically by
 * csilk_ctx_cleanup() and by the panic recovery path.
 *
 * @param c  The request context.
 */
void csilk_ctx_defer_free(csilk_ctx_t* c);

/**
 * @brief Split a URL into path and query-string components.
 *
 * Internal helper.  The returned @p path and @p query point into the
 * original @p url string (the '?' separator is overwritten with NUL).
 *
 * @param  url    Full URL string (will be modified in-place).
 * @param[out] path  Receives a pointer to the path portion inside @p url.
 * @param[out] query Receives a pointer to the query portion inside @p url,
 *                   or nullptr if no query was present.
 */
void csilk_split_url(const char* url, char** path, char** query);
