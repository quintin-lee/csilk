/**
 * @file ws_frame.h
 * @brief WebSocket frame parsing and dispatch (RFC 6455).
 *
 * Provides the core WebSocket frame parser used by the csilk framework
 * to handle incoming WebSocket data on an established connection.
 * @copyright MIT License
 */

#ifndef CSILK_WS_FRAME_H
#define CSILK_WS_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/csilk.h"

/**
 * @brief Parse an incoming WebSocket frame from the raw TCP stream.
 *
 * Processes one or more frames from the receive buffer, dispatches data
 * frames to the registered on_message callback, and handles control frames
 * (ping/pong/close) internally.
 *
 * @param c     Request context (WebSocket mode must be active).
 * @param buf   Raw bytes received from the socket.
 * @param nread Number of bytes in @p buf.
 */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread);

#endif /* CSILK_WS_FRAME_H */
