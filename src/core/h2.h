/**
 * @file h2.h
 * @brief HTTP/2 integration for csilk.
 *
 * @copyright MIT License
 */

#ifndef CSILK_H2_H
#define CSILK_H2_H

#include "core/srv_types.h"

/**
 * @brief Initialize HTTP/2 session for a client connection.
 * @param client The client connection.
 * @return 0 on success, < 0 on failure.
 */
int csilk_h2_init_session(csilk_client_t* client);

/**
 * @brief Process incoming HTTP/2 data.
 * @param client The client connection.
 * @param data   The incoming data buffer.
 * @param len    The length of the incoming data.
 * @return 0 on success, < 0 on failure.
 */
int csilk_h2_process_data(csilk_client_t* client, const uint8_t* data, size_t len);

/**
 * @brief Get an existing stream context or create a new one.
 * @param client    The client connection.
 * @param stream_id The HTTP/2 stream ID.
 * @return Pointer to the context, or nullptr on failure.
 */
csilk_ctx_t* csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id);

/**
 * @brief Free all active HTTP/2 stream contexts for a client.
 * @param client The client connection.
 */
void csilk_h2_free_streams(csilk_client_t* client);

/**
 * @brief Send an HTTP/2 response for a given stream context.
 * @param c The stream context.
 */
void csilk_h2_send_response(csilk_ctx_t* c);

/**
 * @brief Submit an HTTP/2 server push promise for a resource.
 *
 * Sends a PUSH_PROMISE frame and then routes the promised stream's request
 * through the router to generate a response. Must be called from within a
 * request handler on the original client-initiated stream.
 *
 * @param c      The original request context (client-initiated stream).
 * @param method The HTTP method for the pushed resource (usually "GET").
 * @param path   The path of the resource to push (e.g., "/style.css").
 * @return The promised stream ID on success, or < 0 on error.
 */
int32_t csilk_h2_submit_push(csilk_ctx_t* c, const char* method, const char* path);

#endif /* CSILK_H2_H */
