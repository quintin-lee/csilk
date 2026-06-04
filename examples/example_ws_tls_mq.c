/**
 * @file example_ws_tls_mq.c
 * @brief Integrated example demonstrating Secure WebSocket (WSS), TLS,
 * and asynchronous Message Queue (MQ) broadcasting.
 *
 * This example shows how to:
 * 1. Configure a server for HTTPS/TLS using PEM certificates.
 * 2. Implement a WebSocket chat with room management.
 * 3. Use the internal MQ to broadcast messages across all connected clients.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

/**
 * @brief WebSocket message callback.
 *
 * Invoked whenever a client sends a message. It parses the message
 * and broadcasts it to the "chat-room" via the internal MQ.
 */
void
on_ws_chat_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	(void)opcode;
	char* msg = malloc(len + 1);
	if (!msg) {
		return;
	}
	memcpy(msg, payload, len);
	msg[len] = '\0';

	printf("[Chat] Received: %s\n", msg);

	/* Broadcast to all clients in the "lobby" room via MQ */
	csilk_ws_broadcast_room(c, "lobby", msg);

	free(msg);
}

/**
 * @brief WebSocket upgrade handler.
 *
 * Performs the handshake, joins the client to the "lobby" room,
 * and sets the message callback.
 */
void
ws_chat_handler(csilk_ctx_t* c)
{
	csilk_ws_handshake(c);
	if (csilk_is_websocket(c)) {
		printf("[System] WebSocket connected\n");
		csilk_ws_join_room(c, "lobby");
		csilk_set_on_ws_message(c, on_ws_chat_message);

		const char* welcome = "Welcome to the Secure Chat Room!";
		csilk_ws_send(c, (const uint8_t*)welcome, strlen(welcome), 0x1);
	}
}

/**
 * @brief MQ Subscriber callback for custom events.
 */
void
on_system_event(csilk_mq_ctx_t* m_ctx)
{
	size_t len;
	const char* payload = csilk_mq_get_payload(m_ctx, &len);
	if (payload) {
		printf("[MQ Event] System notification: %.*s\n", (int)len, payload);
	}
	csilk_mq_next(m_ctx);
}

/**
 * @brief Simple health check handler.
 */
void
health_handler(csilk_ctx_t* c)
{
	csilk_string(c, 200, "OK");
}

int
main()
{
	/* 1. Setup Server Configuration with TLS */
	csilk_server_config_t config = {0};
	config.enable_tls = 1;
	config.tls_cert_file = "tests/test_cert.pem";
	config.tls_key_file = "tests/test_key.pem";

	/* 2. Setup Routing */
	csilk_router_t* router = csilk_router_new();
	csilk_group_t* root = csilk_group_new(router, "");

	/* Apply standard middlewares */
	csilk_group_use(root, csilk_logger_handler);
	csilk_group_use(root, csilk_recovery_handler);

	/* WebSocket endpoint */
	csilk_GET(root, "/chat", ws_chat_handler);

	/* Simple health check */
	csilk_GET(root, "/health", health_handler);

	/* 3. Initialize Server */
	csilk_server_t* server = csilk_server_new(router);
	csilk_server_set_config(server, &config);

	/* 4. Demonstrate MQ Subscription (System Events) */
	csilk_mq_t* mq = csilk_server_get_mq(server);
	csilk_mq_subscribe(mq, "system.alerts", on_system_event);

	/* 5. Start Server */
	printf("\n🚀 Secure WebSocket Chat Server (WSS) starting...\n");
	printf("   - Port: 8443 (HTTPS/WSS)\n");
	printf("   - Endpoint: wss://localhost:8443/chat\n");
	printf("   - Health Check: https://localhost:8443/health\n\n");
	printf("Note: Using test certificates from 'tests/' directory.\n");
	printf("Connect with a WSS-capable client (e.g., 'wscat -c wss://localhost:8443/chat "
	       "--no-check-certificate')\n\n");

	/* Run server on port 8443 */
	csilk_server_run(server, 8443);

	/* Cleanup */
	csilk_server_free(server);
	csilk_group_free(root);
	csilk_router_free(router);

	return 0;
}
