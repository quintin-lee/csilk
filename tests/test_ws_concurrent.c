#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "core/ctx_internal.h"
#include "core/srv_internal.h"

#define NUM_CLIENTS 8

static csilk_ctx_t* g_clients[NUM_CLIENTS];
static int g_recv_counts[NUM_CLIENTS];
static char g_last_msg[256];

static void
collector(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
	(void)opcode;
	for (int i = 0; i < NUM_CLIENTS; i++) {
		if (c == g_clients[i]) {
			g_recv_counts[i]++;
			if (len < sizeof(g_last_msg)) {
				memcpy(g_last_msg, payload, len);
				g_last_msg[len] = '\0';
			}
			return;
		}
	}
}

static void
test_ws_multi_room_broadcast(void)
{
	printf("Testing WebSocket Multi-Room Broadcast (%d clients)...\n", NUM_CLIENTS);

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);

	for (int i = 0; i < NUM_CLIENTS; i++) {
		g_clients[i] = csilk_test_ctx_new();
		g_clients[i]->server = (struct csilk_server_s*)server;
		csilk_set_on_ws_send(g_clients[i], collector);
		g_recv_counts[i] = 0;
	}

	for (int i = 0; i < NUM_CLIENTS; i++) {
		csilk_ws_join_room(g_clients[i], "lobby");
	}

	csilk_ws_broadcast_room(g_clients[0], "lobby", "hello all");

	for (int i = 0; i < 100; i++) {
		uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	}

	for (int i = 0; i < NUM_CLIENTS; i++) {
		assert(g_recv_counts[i] == 1);
	}
	assert(strcmp(g_last_msg, "hello all") == 0);

	memset(g_recv_counts, 0, sizeof(g_recv_counts));
	csilk_ws_leave_room(g_clients[0], "lobby");
	csilk_ws_broadcast_room(g_clients[1], "lobby", "without client0");

	for (int i = 0; i < 100; i++) {
		uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	}

	assert(g_recv_counts[0] == 0);
	for (int i = 1; i < NUM_CLIENTS; i++) {
		assert(g_recv_counts[i] == 1);
	}
	assert(strcmp(g_last_msg, "without client0") == 0);

	for (int i = 0; i < NUM_CLIENTS; i++) {
		csilk_test_ctx_free(g_clients[i]);
	}
	csilk_server_free(server);
	csilk_router_free(router);

	printf("WebSocket multi-room broadcast: PASS\n");
}

static void
test_ws_rapid_join_leave(void)
{
	printf("Testing WebSocket Rapid Join/Leave Cycles...\n");

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);

	csilk_ctx_t* c1 = csilk_test_ctx_new();
	c1->server = (struct csilk_server_s*)server;
	csilk_set_on_ws_send(c1, collector);
	g_recv_counts[0] = 0;

	csilk_ctx_t* c2 = csilk_test_ctx_new();
	c2->server = (struct csilk_server_s*)server;
	csilk_set_on_ws_send(c2, collector);
	g_recv_counts[1] = 0;

	const char* rooms[] = {"alpha", "beta", "gamma", "delta"};
	int num_rooms = (int)(sizeof(rooms) / sizeof(rooms[0]));

	for (int cycle = 0; cycle < 10; cycle++) {
		for (int r = 0; r < num_rooms; r++) {
			csilk_ws_join_room(c1, rooms[r]);
			csilk_ws_join_room(c2, rooms[r]);
		}
		csilk_ws_broadcast_room(c1, "alpha", "cycle_msg");
		for (int i = 0; i < 50; i++) {
			uv_run(uv_default_loop(), UV_RUN_NOWAIT);
		}
		for (int r = 0; r < num_rooms; r++) {
			csilk_ws_leave_room(c1, rooms[r]);
			csilk_ws_leave_room(c2, rooms[r]);
		}
	}

	csilk_test_ctx_free(c1);
	csilk_test_ctx_free(c2);
	csilk_server_free(server);
	csilk_router_free(router);

	printf("WebSocket rapid join/leave: PASS\n");
}

static void
test_ws_concurrent_rooms(void)
{
	printf("Testing WebSocket Concurrent Rooms (each client in own room)...\n");

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);

	for (int i = 0; i < NUM_CLIENTS; i++) {
		g_clients[i] = csilk_test_ctx_new();
		g_clients[i]->server = (struct csilk_server_s*)server;
		csilk_set_on_ws_send(g_clients[i], collector);
		g_recv_counts[i] = 0;
	}

	char room[32];
	for (int i = 0; i < NUM_CLIENTS; i++) {
		snprintf(room, sizeof(room), "room_%d", i);
		csilk_ws_join_room(g_clients[i], room);
	}

	for (int i = 0; i < NUM_CLIENTS; i++) {
		char msg[64];
		snprintf(msg, sizeof(msg), "msg_from_%d", i);
		snprintf(room, sizeof(room), "room_%d", i);
		csilk_ws_broadcast_room(g_clients[i], room, msg);
	}

	for (int i = 0; i < 200; i++) {
		uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	}

	for (int i = 0; i < NUM_CLIENTS; i++) {
		assert(g_recv_counts[i] == 1);
	}

	for (int i = 0; i < NUM_CLIENTS; i++) {
		csilk_test_ctx_free(g_clients[i]);
	}
	csilk_server_free(server);
	csilk_router_free(router);

	printf("WebSocket concurrent rooms: PASS\n");
}

int
main(void)
{
	test_ws_multi_room_broadcast();
	test_ws_rapid_join_leave();
	test_ws_concurrent_rooms();

	printf("test_ws_concurrent: ALL PASSED\n");
	return 0;
}
