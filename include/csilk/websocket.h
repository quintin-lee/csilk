/**
 * @file websocket.h
 * @brief WebSocket upgrade, framing, and room management for the csilk framework.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_WEBSOCKET_H
#define CSILK_WEBSOCKET_H

#include "csilk/types.h"

/**
 * @brief Perform the WebSocket upgrade handshake (HTTP Upgrade request).
 *
 * Validates the Upgrade, Connection, Sec-WebSocket-Key, and version headers,
 * computes the Sec-WebSocket-Accept response, and sends a 101 Switching
 * Protocols response.  After success, csilk_is_websocket returns 1 and the
 * connection can send/receive frames.
 *
 * @param c  The request context.
 */
void csilk_ws_handshake(csilk_ctx_t* c);

/**
 * @brief Send a WebSocket data frame.
 *
 * Encodes and sends a single WebSocket frame per RFC 6455.  Masks the
 * payload if required (client-to-server masking).
 *
 * @param c       The request context.
 * @param payload Raw data to send.
 * @param len     Byte length of @p payload.
 * @param opcode  WebSocket opcode: 0x1 for text, 0x2 for binary, 0x9 for ping.
 */
void csilk_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode);

/**
 * @brief Send a WebSocket close frame.
 *
 * Initiates the close handshake per RFC 6455 §5.5.1.  After sending, the
 * server waits for the client's close frame before fully closing the TCP
 * connection.
 *
 * @param c           The request context.
 * @param status_code Close status code (e.g., 1000 for normal closure,
 *                    0 to omit the status code from the frame).
 * @param reason      Optional human-readable reason string (may be NULL).
 */
void csilk_ws_close(csilk_ctx_t* c, uint16_t status_code, const char* reason);

/**
 * @name WebSocket Room Management (MQ-based)
 * High-concurrency room broadcasting system leveraging the Message Queue.
 * @{ */

/** @brief Join a WebSocket client to a room.
 *  @param c          Request context (must be a WebSocket).
 *  @param room_name  Name of the room to join. */
void csilk_ws_join_room(csilk_ctx_t* c, const char* room_name);

/** @brief Remove a WebSocket client from a room.
 *  @param c          Request context.
 *  @param room_name  Name of the room to leave. */
void csilk_ws_leave_room(csilk_ctx_t* c, const char* room_name);

/** @brief Broadcast a message to all WebSockets in a room via MQ.
 *  @param c          Request context (used to access the server's MQ).
 *  @param room_name  Room to broadcast to.
 *  @param message    NUL-terminated message string. */
void csilk_ws_broadcast_room(csilk_ctx_t* c, const char* room_name, const char* message);

/** @} */

#endif /* CSILK_WEBSOCKET_H */
