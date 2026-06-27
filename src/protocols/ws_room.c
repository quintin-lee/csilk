/**
 * @file ws_room.c
 * @brief WebSocket room management and broadcast based on MQ.
 *
 * Implements a high-concurrency room broadcasting system for WebSockets.
 * It leverages the internal Message Queue (MQ) as the event bus.
 *
 * Architecture:
 *   - Each room is mapped to an MQ topic: "ws.room.<room_name>".
 *   - A global Room Manager maintains a list of active clients per room.
 *   - A single MQ subscriber per room handles broadcasting to all connected clients.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

typedef struct {
	char* room_name;
	csilk_ctx_t** clients;
	int clients_count;
	int clients_capacity;
} ws_room_t;

typedef struct {
	ws_room_t** rooms;
	int rooms_count;
	int rooms_capacity;
	uv_mutex_t mutex;
} ws_room_manager_t;

static ws_room_manager_t g_room_manager;
static int g_room_manager_initialized = 0;

/** @brief Initialize the global WebSocket room manager singleton.
 *
 * Allocates the room array and initializes the mutex.  Safe to call
 * multiple times — subsequent calls are no-ops.  Must be called from a
 * single thread before any room operations (join/leave/broadcast). */
static void
ws_room_manager_init()
{
	if (g_room_manager_initialized) {
		return;
	}
	memset(&g_room_manager, 0, sizeof(ws_room_manager_t));
	uv_mutex_init(&g_room_manager.mutex);
	g_room_manager_initialized = 1;
}

/** @brief Locate a room by name in the global room manager.
 *
 * Linear scan over the rooms array.  Caller must hold
 * g_room_manager.mutex when calling this function.
 *
 * @param name  Room name to search for.
 * @return Pointer to the ws_room_t, or nullptr if not found. */
static ws_room_t*
find_room(const char* name)
{
	for (int i = 0; i < g_room_manager.rooms_count; i++) {
		if (strcmp(g_room_manager.rooms[i]->room_name, name) == 0) {
			return g_room_manager.rooms[i];
		}
	}
	return nullptr;
}

/** @brief MQ message callback: broadcast a received payload to all clients
 *  in the room.
 *
 * Extracts the room name from the MQ topic ("ws.room.<room>"), iterates
 * the room's client list, and calls csilk_ws_send() on each client with
 * opcode 0x1 (text frame).
 *
 * @param ctx  MQ context providing topic and payload via csilk_mq_get_topic()
 *             and csilk_mq_get_payload(). */
static void
on_room_message(csilk_mq_ctx_t* ctx)
{
	const char* topic = csilk_mq_get_topic(ctx);
	size_t payload_len;
	const uint8_t* payload = (const uint8_t*)csilk_mq_get_payload(ctx, &payload_len);

	if (!topic || strncmp(topic, "ws.room.", 8) != 0) {
		return;
	}

	const char* room_name = topic + 8;

	uv_mutex_lock(&g_room_manager.mutex);
	ws_room_t* room = find_room(room_name);
	if (room) {
		for (int i = 0; i < room->clients_count; i++) {
			// Opcode 1 = Text frame
			csilk_ws_send(room->clients[i], payload, payload_len, 1);
		}
	}
	uv_mutex_unlock(&g_room_manager.mutex);
}

/** @brief Add the current WebSocket client to a named room.
 *
 * Registers the client context in the room's client list.  If the room
 * does not yet exist, it is created and an MQ subscriber is set up on
 * topic "ws.room.<name>" to handle incoming broadcast messages for that
 * room.  Thread-safe via g_room_manager.mutex.
 *
 * @param c          The WebSocket request context to register.
 * @param room_name  Room identifier (may contain alphanumeric and '-',
 *                   '+' characters). */
void
csilk_ws_join_room(csilk_ctx_t* c, const char* room_name)
{
	ws_room_manager_init();
	uv_mutex_lock(&g_room_manager.mutex);

	ws_room_t* room = find_room(room_name);
	if (!room) {
		// Create room
		if (g_room_manager.rooms_count >= g_room_manager.rooms_capacity) {
			int new_cap = g_room_manager.rooms_capacity == 0
					  ? 16
					  : g_room_manager.rooms_capacity * 2;
			g_room_manager.rooms =
			    realloc(g_room_manager.rooms, sizeof(ws_room_t*) * new_cap);
			g_room_manager.rooms_capacity = new_cap;
		}
		room = calloc(1, sizeof(ws_room_t));
		room->room_name = strdup(room_name);
		g_room_manager.rooms[g_room_manager.rooms_count++] = room;

		// Subscribe to MQ topic for this room
		char topic[256];
		snprintf(topic, sizeof(topic), "ws.room.%s", room_name);
		csilk_mq_t* mq = csilk_ctx_get_mq(c);
		if (mq) {
			csilk_mq_subscribe(mq, topic, on_room_message);
		}
	}

	// Add client to room
	for (int i = 0; i < room->clients_count; i++) {
		if (room->clients[i] == c) {
			uv_mutex_unlock(&g_room_manager.mutex);
			return; // Already in room
		}
	}

	if (room->clients_count >= room->clients_capacity) {
		int new_cap = room->clients_capacity == 0 ? 16 : room->clients_capacity * 2;
		room->clients = realloc(room->clients, sizeof(csilk_ctx_t*) * new_cap);
		room->clients_capacity = new_cap;
	}
	room->clients[room->clients_count++] = c;

	uv_mutex_unlock(&g_room_manager.mutex);
}

/** @brief Remove the current WebSocket client from a named room.
 *
 * Swaps the removed client to the end of the array for O(1) deletion,
 * then decrements the count.  If the room does not exist or the client
 * is not found, this is a no-op.  Thread-safe via g_room_manager.mutex.
 *
 * @param c          The WebSocket request context to unregister.
 * @param room_name  Room identifier to leave. */
void
csilk_ws_leave_room(csilk_ctx_t* c, const char* room_name)
{
	ws_room_manager_init();
	uv_mutex_lock(&g_room_manager.mutex);

	ws_room_t* room = find_room(room_name);
	if (room) {
		for (int i = 0; i < room->clients_count; i++) {
			if (room->clients[i] == c) {
				room->clients[i] = room->clients[room->clients_count - 1];
				room->clients_count--;
				break;
			}
		}
	}

	uv_mutex_unlock(&g_room_manager.mutex);
}

/** @brief Publish a message to all clients in a named room via MQ.
 *
 * Publishes the message payload on topic "ws.room.<name>".  The MQ
 * subscriber (on_room_message) picks it up and calls csilk_ws_send()
 * on every client registered in the room.  This decouples the broadcaster
 * from the actual delivery — the publisher does not block on I/O.
 *
 * @param c          The request context (used to obtain the MQ handle).
 * @param room_name  Target room identifier.
 * @param message    UTF-8 text payload to broadcast. */
void
csilk_ws_broadcast_room(csilk_ctx_t* c, const char* room_name, const char* message)
{
	csilk_mq_t* mq = csilk_ctx_get_mq(c);
	if (!mq) {
		return;
	}

	char topic[256];
	snprintf(topic, sizeof(topic), "ws.room.%s", room_name);
	csilk_mq_publish(mq, topic, message, strlen(message));
}
