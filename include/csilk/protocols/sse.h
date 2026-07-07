/**
 * @file sse.h
 * @brief Server-Sent Events (SSE) functions for the csilk framework.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_SSE_H
#define CSILK_SSE_H

#include "csilk/core/types.h"

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
 * Formats and flushes one SSE message.  If @p event is nullptr and @p data is
 * non-nullptr, a default "message" event is sent.  If @p data is nullptr, a
 * comment line (starting with ":") is written.
 *
 * @param c     The request context.
 * @param event Optional event type string (e.g., "update"), or nullptr.
 * @param data  Event data string, or nullptr to send a comment line.
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

#endif /* CSILK_SSE_H */
