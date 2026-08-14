#pragma once
/**
 * @file response.h
 * @brief HTTP response functions for the csilk framework.
 *
 * Provides functions to write status codes, body content, headers,
 * cookies, JSON, redirects, file responses, and streaming responses.
 *
 * @version 0.4.0
 * @copyright MIT License
 */

#include "csilk/core/types.h"

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
                      const char*  name,
                      const char*  value,
                      int          max_age,
                      const char*  path,
                      const char*  domain,
                      int          secure,
                      int          http_only);

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
void csilk_json(csilk_ctx_t* c, int status, csilk_json_t* json);

/**
 * @brief Send a raw JSON string response without AST building or heap allocation.
 *
 * @param c        The request context.
 * @param status   HTTP status code.
 * @param json_str Static or unmanaged JSON string.
 */
void csilk_json_string(csilk_ctx_t* c, int status, const char* json_str);

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

/** @brief Convenience macro for sending a reflected struct as JSON response.
 *  Wraps csilk_json_reflect, automatically stringifying the type name.
 *  @param c The request context.
 *  @param status HTTP status code.
 *  @param type The struct type (used with #type to get the name).
 *  @param ptr Pointer to the struct instance. */
#define csilk_json_t(c, status, type, ptr) csilk_json_reflect(c, status, #type, ptr)

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
 * @brief Serve a specific file from the local filesystem.
 *
 * Like csilk_static, this function offloads file I/O to a worker thread and
 * uses zero-copy transmission (sendfile).
 *
 * @param c          The request context.
 * @param file_path  Absolute or relative path to the file.
 */
void csilk_file(csilk_ctx_t* c, const char* file_path);

/* --- Streaming Response (Chunked Transfer Encoding & Backpressure) --- */

#define CSILK_WRITE_HWM_DEFAULT (64 * 1024)
#define CSILK_WRITE_LWM_DEFAULT (16 * 1024)
#define CSILK_WRITE_MAX_BUFFER_DEFAULT (4 * 1024 * 1024)

/**
 * @brief Write a chunk to the response stream (chunked transfer encoding) with backpressure.
 *
 * The first call sends the HTTP response headers with
 * Transfer-Encoding: chunked.  Subsequent calls append chunked frames.
 * The handler MUST set async mode (csilk_ctx_set_async(c, 1)) before calling
 * this and MUST NOT use csilk_string, csilk_json, etc.
 *
 * @param c    The request context.
 * @param data Raw data for the chunk.
 * @param len  Byte length of @p data.
 * @return 1 if written and write queue is healthy (writable),
 *         0 if backpressure was triggered (queue >= high water mark; caller should pause),
 *        -1 on error or if max write buffer exceeded.
 */
int csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len);

/**
 * @brief Finalise a chunked streaming response.
 *
 * Sends the terminal (zero-length) chunk and any trailers.  The connection
 * is then kept alive or closed according to the HTTP keep-alive header.
 *
 * @param c  The request context.
 */
void csilk_response_end(csilk_ctx_t* c);

/**
 * @brief Query current pending outbound bytes queued for this connection.
 *
 * @param c The request context.
 * @return Number of queued bytes waiting to be transmitted.
 */
size_t csilk_response_get_write_queue_size(csilk_ctx_t* c);

/**
 * @brief Check if the connection is currently writable (below high water mark).
 *
 * @param c The request context.
 * @return 1 if writable, 0 if paused / backpressure active.
 */
int csilk_response_is_writable(csilk_ctx_t* c);

/**
 * @brief Configure backpressure watermarks for this connection.
 *
 * @param c               The request context.
 * @param high_water_mark High water mark in bytes (0 to disable pause threshold).
 * @param low_water_mark  Low water mark in bytes (threshold to resume/trigger on_drain).
 * @param max_buffer_size Hard buffer limit in bytes (0 to disable hard limit).
 */
void csilk_response_set_watermarks(csilk_ctx_t* c,
                                   size_t       high_water_mark,
                                   size_t       low_water_mark,
                                   size_t       max_buffer_size);

/**
 * @brief Register a drain callback to be invoked when the outbound queue drains below low water mark.
 *
 * @param c         The request context.
 * @param on_drain  Drain callback function pointer.
 * @param user_data User data passed to @p on_drain.
 */
void csilk_response_on_drain(csilk_ctx_t* c,
                             void (*on_drain)(csilk_ctx_t* c, void* user_data),
                             void* user_data);

/**
 * @brief Signal the server to push a resource via HTTP/2 server push.
 *
 * Sends a PUSH_PROMISE frame to the client and dispatches the pushed
 * resource's request through the router. The pushed response is sent
 * automatically on the promised stream. Must be called from within an
 * HTTP/2 request handler; on HTTP/1.1 connections this is a no-op.
 *
 * @param c      The current request context.
 * @param method The HTTP method for the pushed resource (typically "GET").
 * @param path   The path of the pushed resource (e.g., "/style.css").
 * @return The promised stream ID on success, or < 0 on error.
 */
int32_t csilk_push_promise(csilk_ctx_t* c, const char* method, const char* path);
