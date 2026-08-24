/**
 * @file response.c
 * @brief HTTP response writing — status, headers, JSON, redirect, chunked streaming.
 *
 * Implements all response output functions: status/string/json/redirect,
 * header management (Set-Cookie), and chunked transfer-encoding streaming
 * (csilk_response_write / csilk_response_end).
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "csilk/core/json.h"
#include "csilk/reflection/reflect.h"
#include "../ctx/ctx_internal.h"
#include "../internal/srv_impl.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "../primitives/header_map.h"
#include "csilk/http/h2.h"

/* --- Status & string --- */

/** @brief Set the HTTP status code for the response.
 *
 * @param c      The request context.
 * @param status HTTP status code (e.g., 200, 404, 500).
 * @note Also accessible via CSILK_STATUS_OK, CSILK_STATUS_NOT_FOUND, etc. */
void
csilk_status(csilk_ctx_t* c, int status)
{
    if (!c) {
        return;
    }
    c->response.status = status;
}

/** @brief Send a plain text response with the given status code.
 *
 * Copies the message string to the request arena (when available) or the
 * heap, sets the Content-Type header to text/plain, and marks the response
 * ready for sending.
 *
 * @param c      The request context.
 * @param status HTTP status code.
 * @param msg    Plain text body (may be NULL).
 * @note Ownership: when arena is unavailable, the strdup'd copy is freed
 *       automatically during csilk_ctx_cleanup(). Safe to pass NULL for msg. */
void
csilk_string(csilk_ctx_t* c, int status, const char* msg)
{
    if (!c) {
        CSILK_LOG_E("Response: csilk_string called with null context");
        return;
    }
    c->response.status = status;
    csilk_response_body_release(c);

    size_t msg_len = msg ? strlen(msg) : 0;
    if (c->arena) {
        c->response.body = msg ? csilk_arena_strdup(c->arena, msg) : NULL;
        c->response.body_len = msg_len;
        c->response.body_ownership = msg ? CSILK_OWN_ARENA : CSILK_OWN_NONE;
    } else {
        char* body = msg ? strdup(msg) : NULL;
        c->response.body = body;
        c->response.body_len = body ? msg_len : 0;
        c->response.body_ownership = body ? CSILK_OWN_HEAP : CSILK_OWN_NONE;
    }
}

/* --- Response header setters --- */

/** @brief Set a response header (replaces any existing value).
 *
 * @param c     The request context.
 * @param key   Header name.
 * @param value Header value. */
void
csilk_set_header(csilk_ctx_t* c, const char* key, const char* value)
{
    map_set(c, &c->response.headers, key, value);
}

/** @brief Add a response header (allows multiple values for the same key).
 *
 * Unlike csilk_set_header() which replaces, this appends to any existing
 * value for the key. Used for Set-Cookie and other multi-value headers.
 *
 * @param c     The request context.
 * @param key   Header name.
 * @param value Header value. */
void
csilk_add_header(csilk_ctx_t* c, const char* key, const char* value)
{
    map_add(c, &c->response.headers, key, value);
}

/* --- Redirect --- */

/** @brief Send an HTTP redirect response.
 *
 * Sets the Location header and sends an immediate redirect response with
 * the specified 3xx status code. Aborts the handler chain.
 *
 * @param c        The request context.
 * @param status   HTTP status code (300-308, defaults to 302 if out of range).
 * @param location Target URL for the redirect.
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

/* --- Cookie --- */

/** @brief Set a cookie in the response via Set-Cookie header.
 *
 * Constructs a properly formatted Set-Cookie header with the given name,
 * value, and attributes. The cookie is added using csilk_add_header() so
 * multiple cookies can be set on the same response.
 *
 * @param c         The request context.
 * @param name      Cookie name (cannot be NULL).
 * @param value     Cookie value (cannot be NULL).
 * @param max_age   Cookie Max-Age in seconds. Pass 0 to omit, negative for
 *                  immediate expiry (Max-Age=0), positive for a future expiry.
 * @param path      Cookie path (pass NULL for default "/").
 * @param domain    Cookie domain (pass NULL to omit).
 * @param secure    If non-zero, adds the Secure flag.
 * @param http_only If non-zero, adds the HttpOnly flag.
 * @note The cookie is arena-allocated. The name+value and attribute strings
 *       should not contain characters that break cookie formatting. */
void
csilk_set_cookie(csilk_ctx_t* c,
                 const char*  name,
                 const char*  value,
                 int          max_age,
                 const char*  path,
                 const char*  domain,
                 int          secure,
                 int          http_only)
{
    if (!c || !c->arena) {
        return;
    }
    size_t buf_size = strlen(name) + strlen(value) + 256;
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

/* --- JSON --- */

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
csilk_json(csilk_ctx_t* c, int status, csilk_json_t* json)
{
    if (!c || !json) {
        if (!c) {
            CSILK_LOG_E("Response: csilk_json called with null context");
        }
        if (!json) {
            CSILK_LOG_E("Response: csilk_json called with null json");
        }
        return;
    }

    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");

    size_t out_len = 0;
    char*  body = csilk_json_serialize(json, &out_len);
    csilk_json_free(json);

    csilk_response_body_release(c);

    if (body) {
        c->response.body = body;
        c->response.body_len = out_len;
        c->response.body_ownership = CSILK_OWN_HEAP;
    }
}

/**
 * @brief Set a JSON string as the response body with an application/json type.
 *
 * Sets the response status, assigns the Content-Type header, frees any prior
 * managed body, and points the response body at @p json_str without copying or
 * taking ownership (ownership set to CSILK_OWN_BORROWED).
 *
 * @param[in] c         Request context (must not be NULL).
 * @param[in] status    HTTP status code to send.
 * @param[in] json_str  NUL-terminated JSON text (must not be NULL); must remain
 *                      valid until the response is written.
 */
void
csilk_json_string(csilk_ctx_t* c, int status, const char* json_str)
{
    if (!c || !json_str) {
        return;
    }

    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");

    csilk_response_body_release(c);

    c->response.body = json_str;
    c->response.body_len = strlen(json_str);
    c->response.body_ownership = CSILK_OWN_BORROWED;
}

/** @brief Send a JSON error response containing an "error" field (no-heap
 *  hot path).
 *
 * Uses a bounded stack buffer for small error messages (the common case).
 * Falls back to cJSON if the message is too large for the stack buffer.
 *
 * @param c       The request context.
 * @param status  HTTP status code (e.g., 400, 500).
 * @param message Error description string. */
void
csilk_json_error(csilk_ctx_t* c, int status, const char* message)
{
    if (!c) {
        return;
    }

    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");

    csilk_response_body_release(c);

    char   stack_buf[256];
    size_t needed = strlen(message ? message : "") + 32;

    if (needed < sizeof(stack_buf)) {
        int n = snprintf(stack_buf,
                         sizeof(stack_buf),
                         "{\"error\":\"%s\"}",
                         message ? message : "Unknown error");
        if (n > 0 && (size_t)n < sizeof(stack_buf)) {
            if (c->arena) {
                c->response.body = csilk_arena_strndup(c->arena, stack_buf, (size_t)n);
                c->response.body_len = (size_t)n;
                c->response.body_ownership = CSILK_OWN_ARENA;
                return;
            }
        }
    }

    csilk_json_t* json = csilk_json_object();
    csilk_json_add_string(json, "error", message ? message : "Unknown error");
    char* body = csilk_json_serialize(json, NULL);
    csilk_json_free(json);

    if (body) {
        c->response.body = body;
        c->response.body_len = strlen(body);
        c->response.body_ownership = CSILK_OWN_HEAP;
    }
}

/**
 * @brief Serialize a struct into the response body via reflection metadata.
 *
 * Automatically infers the output type from the matched handler if type_name
 * is omitted. Returns HTTP 500 if reflection serialization fails.
 *
 * @param[in] c         Request context (must not be NULL).
 * @param[in] status    HTTP status code to set.
 * @param[in] type_name Registered struct name, or NULL to infer from handler.
 * @param[in] ptr       Pointer to the struct instance (must not be NULL).
 */
void
csilk_json_marshal_response(csilk_ctx_t* c, int status, const char* type_name, const void* ptr)
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
    size_t body_len = 0;
    char*  json_str = csilk_json_marshal_arena(c->arena, type_name, ptr, &body_len);
    if (json_str) {
        c->response.status = status;
        csilk_set_header(c, "Content-Type", "application/json");
        csilk_response_body_release(c);
        c->response.body = json_str;
        c->response.body_len = body_len;
        c->response.body_ownership = (json_str && c->arena) ? CSILK_OWN_ARENA : CSILK_OWN_HEAP;
    }
}

/** @brief Send a JSON response from a registered struct via reflection. */
void
csilk_json_reflect(csilk_ctx_t* c, int status, const char* type_name, const void* ptr)
{
    csilk_json_marshal_response(c, status, type_name, ptr);
}

/* --- Streaming / chunked response --- */

/** @brief Completion callback for streamed/chunked writes.
 *
 * Logs write errors and frees the per-write request struct and its associated
 * data buffer once the asynchronous write finishes.
 *
 * @param[in] req    The completed write request.
 * @param[in] status Write status (< 0 indicates an error). */
static void
on_stream_write(csilk_io_write_t* req, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Stream write error: %s", csilk_io_strerror(status));
    }
    csilk_client_t* client = NULL;
    if (req->handle) {
        client = (csilk_client_t*)req->handle->data;
    }
    if (req->data) {
        free(req->data);
    }
    free(req);

    if (client) {
        _csilk_client_pending_io_dec(client);
        csilk_client_unref(client);
    }
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

/** @brief Write completion callback for a terminal chunk — closes the
 * connection.
 *
 * Frees the write buffer and request structure, then closes the underlying
 * handle. This is used for the final chunk of a streaming response.
 *
 * @param req    The completed write request (freed by this callback).
 * @param status UV status code (negative indicates error). */
static void
on_stream_end_write(csilk_io_write_t* req, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Stream end write error: %s", csilk_io_strerror(status));
    }
    if (req->data) {
        free(req->data);
    }
    if (req->handle) {
        csilk_io_close((csilk_io_handle_t*)req->handle, NULL);
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
    if (!c || c->conn_closed || !c->_internal_client) {
        return -1;
    }

    csilk_response_body_release(c);

    int         status = c->response.status ? c->response.status : CSILK_STATUS_OK;
    const char* status_text = get_status_text(status);
    int         want_close = client_wants_close(c);
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

    size_t            response_len = (size_t)header_len + custom_headers_len + 2;
    csilk_io_write_t* req = malloc(sizeof(csilk_io_write_t));
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
            pos += snprintf(
                write_base + pos, response_len + 1 - (size_t)pos, "%s: %s\r\n", h->key, h->value);
        }
    }

    snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n");

    csilk_io_buf_t buf = csilk_io_buf_init(write_base, (size_t)pos + 2);
    req->data = write_base;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_client_ref(client);
    _csilk_client_pending_io_inc(client);
    int r = csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_stream_write);
    if (r < 0) {
        _csilk_client_pending_io_dec(client);
        csilk_client_unref(client);
        free(write_base);
        free(req);
        return -1;
    }
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
    int  size_len = snprintf(size_buf, sizeof(size_buf), "%zx\r\n", len);
    if (size_len <= 0) {
        return;
    }

    size_t total = (size_t)size_len + len + 2;
    char*  buf = malloc(total);
    if (!buf) {
        return;
    }

    memcpy(buf, size_buf, (size_t)size_len);
    if (len > 0 && data) {
        memcpy(buf + (size_t)size_len, data, len);
    }
    buf[(size_t)size_len + len] = '\r';
    buf[(size_t)size_len + len + 1] = '\n';

    _csilk_send_data_owned(c, buf, total);
}

/** @brief Write data to a streaming response using chunked transfer encoding with backpressure.
 *
 * On the first call, automatically sends chunked headers (status line +
 * Transfer-Encoding: chunked). Subsequent calls append data chunks.
 * Sets the response to async mode so the framework does not auto-send
 * the response after the handler returns.
 *
 * @param c    Request context.
 * @param data Payload data to write.
 * @param len  Length of data in bytes.
 * @return 1 if written and write queue is healthy (writable),
 *         0 if backpressure was triggered (queue >= high water mark; caller should pause),
 *        -1 on error or if max write buffer exceeded.
 * @note After all data has been written, call csilk_response_end() to send
 *       the terminal chunk and finalize the response. */
int
csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return -1;
    }

    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_conn_set_state(client, CSILK_CONN_STREAMING);
    size_t q = _csilk_client_get_write_queue_size(client);

    if (c->max_write_buffer_size > 0 && q + len > c->max_write_buffer_size) {
        CSILK_LOG_E("Response: max write buffer exceeded (%zu + %zu > %zu), dropping write",
                    q,
                    len,
                    c->max_write_buffer_size);
        return -1;
    }

    if (!c->response_started) {
        if (send_chunked_headers(c) != 0) {
            return -1;
        }
        c->response_started = 1;
        c->is_async = 1;
    }

    if (len == 0) {
        return (c->write_high_water_mark == 0 || q < c->write_high_water_mark) ? 1 : 0;
    }

    write_chunk_frame(c, data, len);

    q = _csilk_client_get_write_queue_size(client);
    if (c->write_high_water_mark > 0 && q >= c->write_high_water_mark) {
        c->write_paused = 1;
        return 0;
    }

    return 1;
}

/** @brief Query current pending outbound bytes queued for this connection.
 *
 * @param c The request context.
 * @return Number of queued bytes waiting to be transmitted. */
size_t
csilk_response_get_write_queue_size(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return 0;
    }
    return _csilk_client_get_write_queue_size((csilk_client_t*)c->_internal_client);
}

/** @brief Check if the connection is currently writable (below high water mark).
 *
 * @param c The request context.
 * @return 1 if writable, 0 if paused / backpressure active. */
int
csilk_response_is_writable(csilk_ctx_t* c)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return 0;
    }
    if (c->write_paused) {
        return 0;
    }
    size_t q = csilk_response_get_write_queue_size(c);
    return (c->write_high_water_mark == 0 || q < c->write_high_water_mark);
}

/** @brief Configure backpressure watermarks for this connection.
 *
 * @param c               The request context.
 * @param high_water_mark High water mark in bytes (0 to disable pause threshold).
 * @param low_water_mark  Low water mark in bytes (threshold to resume/trigger on_drain).
 * @param max_buffer_size Hard buffer limit in bytes (0 to disable hard limit). */
void
csilk_response_set_watermarks(csilk_ctx_t* c,
                              size_t       high_water_mark,
                              size_t       low_water_mark,
                              size_t       max_buffer_size)
{
    if (!c) {
        return;
    }
    c->write_high_water_mark = high_water_mark;
    c->write_low_water_mark =
        (low_water_mark <= high_water_mark) ? low_water_mark : (high_water_mark / 2);
    c->max_write_buffer_size = max_buffer_size;
}

/** @brief Register a drain callback to be invoked when the outbound queue drains below low water mark.
 *
 * @param c         The request context.
 * @param on_drain  Drain callback function pointer.
 * @param user_data User data passed to @p on_drain. */
void
csilk_response_on_drain(csilk_ctx_t* c,
                        void (*on_drain)(csilk_ctx_t* c, void* user_data),
                        void* user_data)
{
    if (!c) {
        return;
    }
    c->on_drain = on_drain;
    c->on_drain_data = user_data;
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
    if (!c || c->conn_closed || !c->_internal_client) {
        return;
    }

    if (!c->response_started) {
        send_chunked_headers(c);
        c->is_async = 1;
    }

    _csilk_send_data(c, (const uint8_t*)"0\r\n\r\n", 5);

    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client && client->protocol == CSILK_PROTO_HTTP1) {
        int keep_alive = !client_wants_close(c);
        _csilk_handle_post_response(client, keep_alive);
    }
}

/* --- HTTP/2 Server Push --- */

/** @brief Signal server push for a resource (HTTP/2 only).
 *
 * Delegates to the HTTP/2 implementation. On HTTP/1.1 connections
 * this is a safe no-op. See src/core/h2.c:csilk_h2_submit_push for
 * the HTTP/2-level implementation.
 *
 * @param c      The request context.
 * @param method The HTTP method for the pushed resource.
 * @param path   The path of the resource to push.
 * @return The promised stream ID on success, or < 0 on error. */
int32_t
csilk_push_promise(csilk_ctx_t* c, const char* method, const char* path)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return -1;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client->protocol != CSILK_PROTO_HTTP2) {
        return -1;
    }
    return csilk_h2_submit_push(c, method, path);
}
