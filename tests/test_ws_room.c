#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/core/ctx_types.h"
#include "csilk/core/srv_types.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int messages_received = 0;
static char last_message[256];

static void
on_ws_send(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	(void)c;
	(void)opcode;
	memcpy(last_message, payload, len);
	last_message[len] = '\0';
	messages_received++;
}

void
test_ws_room_broadcast()
{
	printf("Testing WebSocket Room Broadcast (via MQ)...\n");

	// 1. Setup server and two contexts
	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);

	csilk_ctx_t* c1 = csilk_test_ctx_new();
	csilk_ctx_t* c2 = csilk_test_ctx_new();

	// In a real server, these would be set during connection
	c1->server = (struct csilk_server_s*)server;
	c2->server = (struct csilk_server_s*)server;

	// Use on_ws_send for intercepting outgoing frames
	csilk_set_on_ws_send(c1, on_ws_send);
	csilk_set_on_ws_send(c2, on_ws_send);

	// 2. Join room
	csilk_ws_join_room(c1, "lobby");
	csilk_ws_join_room(c2, "lobby");

	// 3. Broadcast
	messages_received = 0;
	csilk_ws_broadcast_room(c1, "lobby", "hello room");

	// 4. Run loop to process MQ
	for (int i = 0; i < 100 && messages_received < 2; i++) {
		uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	}

	assert(messages_received == 2);
	assert(strcmp(last_message, "hello room") == 0);

	// 5. Leave room
	csilk_ws_leave_room(c1, "lobby");
	messages_received = 0;
	csilk_ws_broadcast_room(c2, "lobby", "only c2");

	for (int i = 0; i < 100 && messages_received < 1; i++) {
		uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	}

	assert(messages_received == 1);
	assert(strcmp(last_message, "only c2") == 0);

	csilk_test_ctx_free(c1);
	csilk_test_ctx_free(c2);
	csilk_server_free(server);
	csilk_router_free(router);

	printf("test_ws_room_broadcast passed!\n");
}

int
main()
{
	test_ws_room_broadcast();
	return 0;
}
