/**
 * @file example_websocket.c
 * @brief Standalone WebSocket server example demonstrating handshake, 
 *        echoing, and room-based chat broadcasting using csilk.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

/**
 * @brief Callback invoked when a WebSocket message is received.
 */
static void
on_ws_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	/* Treat the payload as a string for logging (make a copy to safely null-terminate) */
	char* msg = malloc(len + 1);
	if (!msg) {
		return;
	}
	memcpy(msg, payload, len);
	msg[len] = '\0';

	printf("[WS Message] Received (opcode 0x%x): %s\n", opcode, msg);

	/* Check if the message is a command to broadcast to the room */
	if (strncmp(msg, "/broadcast ", 11) == 0) {
		const char* broadcast_msg = msg + 11;
		printf("[WS Room] Broadcasting to 'chat-room': %s\n", broadcast_msg);
		csilk_ws_broadcast_room(c, "chat-room", broadcast_msg);
	} else {
		/* Otherwise, echo the message back to the sender */
		printf("[WS Echo] Echoing back: %s\n", msg);
		csilk_ws_send(c, (const uint8_t*)msg, len, opcode);
	}

	free(msg);
}

/**
 * @brief Handler for GET /ws endpoint.
 * Upgrades the connection and sets up message processing.
 */
static void
ws_handler(csilk_ctx_t* c)
{
	/* Perform the handshake to upgrade HTTP to WebSocket */
	csilk_ws_handshake(c);

	if (csilk_is_websocket(c)) {
		printf("[WS System] Client successfully upgraded to WebSocket\n");

		/* Join a built-in chat room */
		csilk_ws_join_room(c, "chat-room");

		/* Set callback for incoming messages */
		csilk_set_on_ws_message(c, on_ws_message);

		/* Send a welcome message */
		const char* welcome =
		    "Connected to csilk WebSocket! Use '/broadcast <msg>' to chat.";
		csilk_ws_send(
		    c, (const uint8_t*)welcome, strlen(welcome), 0x1); /* 0x1 is text frame */
	} else {
		printf("[WS System] Failed to upgrade client\n");
	}
}

int
main(void)
{
	/* Initialize the router */
	csilk_router_t* router = csilk_router_new();
	csilk_group_t* root = csilk_group_new(router, "");

	/* Apply standard logging and recovery middleware */
	csilk_group_use(root, csilk_logger_handler);
	csilk_group_use(root, csilk_recovery_handler);

	/* Register WebSocket route */
	csilk_GET(root, "/ws", ws_handler);

	/* Create and configure the server */
	csilk_server_t* server = csilk_server_new(router);
	if (!server) {
		fprintf(stderr, "Failed to create csilk server\n");
		return 1;
	}

	printf("\n🚀 Starting WebSocket server on ws://localhost:8080/ws\n");
	printf("   - Connect using 'wscat -c ws://localhost:8080/ws'\n");

	/* Start listening and running the event loop on port 8080 */
	csilk_server_run(server, 8080);

	/* Clean up */
	csilk_server_free(server);
	csilk_group_free(root);
	csilk_router_free(router);

	return 0;
}
