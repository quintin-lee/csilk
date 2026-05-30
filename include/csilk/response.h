/**
 * @file response.h
 * @brief HTTP response functions for the csilk framework.
 *
 * Provides functions to write status codes, body content, headers,
 * cookies, JSON, redirects, file responses, and streaming responses.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_RESPONSE_H
#define CSILK_RESPONSE_H

#include "csilk/types.h"

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
 * @param path      Cookie path scope, or nullptr for "/".
 * @param domain    Cookie domain scope, or nullptr for current host.
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
 * @param location The destination URL.  Must not be nullptr.
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

#endif /* CSILK_RESPONSE_H */
