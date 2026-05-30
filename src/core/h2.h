/**
 * @file h2.h
 * @brief HTTP/2 integration for csilk.
 *
 * @copyright MIT License
 */

#ifndef CSILK_H2_H
#define CSILK_H2_H

#include "csilk/core/srv_types.h"

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
 * @return Pointer to the context, or NULL on failure.
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

#endif /* CSILK_H2_H */
