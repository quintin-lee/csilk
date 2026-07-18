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
#include "cJSON.h"
#include "csilk/reflection/reflect.h"
#include "../ctx/ctx_internal.h"
#include "../internal/srv_impl.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "../primitives/header_map.h"
#include "../http/h2.h"

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

/** @brief Set the response body as plain text with a status code.
 *
 * If the context has an arena allocator, the body string is duplicated into
 * arena memory. Otherwise, it falls back to strdup() and marks the body as
 * managed (so it will be freed during cleanup). The Content-Type header is
 * NOT set automatically — callers should set it explicitly if needed.
 *
 * @param c      The request context.
 * @param status HTTP status code for the response.
 * @param msg    Plain text body (may be nullptr).
 * @note Ownership: when arena is unavailable, the strdup'd copy is freed
 *       automatically during csilk_ctx_cleanup(). Safe to pass nullptr for msg. */
void
csilk_string(csilk_ctx_t* c, int status, const char* msg)
{
    if (!c) {
        CSILK_LOG_E("Response: csilk_string called with null context");
        return;
    }
    c->response.status = status;
    size_t msg_len = msg ? strlen(msg) : 0;
    if (c->arena) {
        c->response.body = msg ? csilk_arena_strdup(c->arena, msg) : nullptr;
        c->response.body_len = msg_len;
        c->response.body_is_managed = 0;
    } else {
        if (c->response.body && c->response.body_is_managed) {
            free((void*)c->response.body);
        }
        char* body = msg ? strdup(msg) : nullptr;
        c->response.body = body;
        c->response.body_len = body ? msg_len : 0;
        c->response.body_is_managed = body ? 1 : 0;
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
 * @param name      Cookie name (cannot be nullptr).
 * @param value     Cookie value (cannot be nullptr).
 * @param max_age   Cookie Max-Age in seconds. Pass 0 to omit, negative for
 *                  immediate expiry (Max-Age=0), positive for a future expiry.
 * @param path      Cookie path (pass nullptr for default "/").
 * @param domain    Cookie domain (pass nullptr to omit).
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
csilk_json(csilk_ctx_t* c, int status, cJSON* json)
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

    if (c->response.body && c->response.body_is_managed) {
        free((void*)c->response.body);
        c->response.body = nullptr;
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

void
csilk_json_string(csilk_ctx_t* c, int status, const char* json_str)
{
    if (!c || !json_str) {
        return;
    }

    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");

    if (c->response.body && c->response.body_is_managed) {
        free((void*)c->response.body);
        c->response.body = nullptr;
        c->response.body_is_managed = 0;
    }

    c->response.body = json_str;
    c->response.body_len = strlen(json_str);
    c->response.body_is_managed = 0;
}

/** @brief Send a JSON error response containing an "error" field (no-heap
 *  hot path).
 *
 * Uses a bounded stack buffer for small error messages (the common case).
 * Falls back to cJSON if the message is too large for the stack buffer.
 *
 * @param c       The request context.
 * @param status  HTTP status code.
 * @param message Error message string (if nullptr, "Unknown error" is used). */
void
csilk_json_error(csilk_ctx_t* c, int status, const char* message)
{
    if (!c || !c->arena) {
        return;
    }

    if (!message) {
        message = "Unknown error";
    }

    char                 buf[256];
    csilk_bounded_json_t j;
    csilk_bounded_json_error(&j, buf, sizeof(buf), message);

    csilk_set_header(c, "Content-Type", "application/json");
    c->response.status = status;

    const char* body = csilk_bounded_json_str(&j);
    size_t      body_len = csilk_bounded_buf_len(&j.buf);

    if (csilk_bounded_json_overflow(&j)) {
        /* Fall back to cJSON for unusually long messages */
        cJSON* err = cJSON_CreateObject();
        if (err) {
            cJSON_AddStringToObject(err, "error", message);
            csilk_json(c, status, err);
        }
        return;
    }

    char* arena_body = csilk_arena_strndup(c->arena, body, body_len);
    if (arena_body) {
        csilk_set_response_body(c, arena_body, body_len, 0);
    }
}

/* --- JSON reflect --- */

/** @brief Send a JSON response from a registered struct via reflection.
 *
 * Serializes the provided struct to JSON using the csilk reflection engine
 * and sends it as the HTTP response. If @p type_name is nullptr, the type is
 * inferred from the current handler's output_type metadata.
 *
 * @param c         The request context.
 * @param status    HTTP status code.
 * @param type_name Registered type name, or nullptr to infer from route metadata.
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
    size_t body_len = 0;
    char*  json_str = csilk_json_marshal_arena(c->arena, type_name, ptr, &body_len);
    if (json_str) {
        c->response.status = status;
        csilk_set_header(c, "Content-Type", "application/json");
        if (c->response.body && c->response.body_is_managed) {
            free((void*)c->response.body);
        }
        c->response.body = json_str;
        c->response.body_len = body_len;
        c->response.body_is_managed = c->arena ? 0 : 1;
    }
}

/* --- Streaming / chunked response --- */

static void
on_stream_write(csilk_io_write_t* req, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Stream write error: %s", csilk_io_strerror(status));
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
        csilk_io_close((csilk_io_handle_t*)req->handle, nullptr);
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
 * @return 0 on success, -1 on allocation failure or nullptr input.
 * @note Sets c->response_started = 1 on success. */
static int
send_chunked_headers(csilk_ctx_t* c)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return -1;
    }

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

    int header_len = snprintf(nullptr,
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
    csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_stream_write);
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
    if (!c || c->conn_closed || !c->_internal_client) {
        return;
    }

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
