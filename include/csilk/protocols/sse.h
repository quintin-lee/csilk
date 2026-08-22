#pragma once
/**
 * @file sse.h
 * @brief Server-Sent Events (SSE) functions for the csilk framework.
 *
 * @version 0.5.1
 * @copyright MIT License
 */

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
 * @brief Send an SSE event (or comment) to the client with backpressure awareness.
 *
 * Formats and flushes one SSE message.  If @p event is NULL and @p data is
 * non-NULL, a default "message" event is sent.  If @p data is NULL, a
 * comment line (starting with ":") is written.
 *
 * @param c     The request context.
 * @param event Optional event type string (e.g., "update"), or NULL.
 * @param data  Event data string, or NULL to send a comment line.
 * @return 1 if sent and write queue is healthy (writable),
 *         0 if backpressure was triggered (queue >= high water mark; caller should pause),
 *        -1 on error or if max write buffer exceeded.
 */
int csilk_sse_send(csilk_ctx_t* c, const char* event, const char* data);

/**
 * @brief Close the SSE connection.
 *
 * Sends any remaining buffered data and closes the TCP connection.
 *
 * @param c  The request context.
 */
void csilk_sse_close(csilk_ctx_t* c);
