/**
 * @file ws_room.c
 * @brief WebSocket room management and broadcast based on MQ with Copy-On-Write (COW).
 *
 * Implements a high-concurrency room broadcasting system for WebSockets using COW
 * snapshots to avoid lock holding during network writes.
 *
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"

typedef struct {
    csilk_ctx_t** clients;
    int           count;
} ws_room_snapshot_t;

typedef struct {
    char*                        room_name;
    _Atomic(ws_room_snapshot_t*) snapshot;
    csilk_mutex_t                mutex;
} ws_room_t;

typedef struct {
    ws_room_t**   rooms;
    int           rooms_count;
    int           rooms_capacity;
    csilk_mutex_t mutex;
} ws_room_manager_t;

static ws_room_manager_t g_room_manager;
static int               g_room_manager_initialized = 0;

static void
ws_room_manager_cleanup(void)
{
    if (!g_room_manager_initialized) {
        return;
    }
    csilk_mutex_lock(&g_room_manager.mutex);
    for (int i = 0; i < g_room_manager.rooms_count; i++) {
        ws_room_t* room = g_room_manager.rooms[i];
        if (room) {
            ws_room_snapshot_t* snap = atomic_load(&room->snapshot);
            if (snap) {
                if (snap->clients) {
                    free(snap->clients);
                }
                free(snap);
            }
            free(room->room_name);
            csilk_mutex_destroy(&room->mutex);
            free(room);
        }
    }
    free(g_room_manager.rooms);
    g_room_manager.rooms = NULL;
    g_room_manager.rooms_count = 0;
    g_room_manager.rooms_capacity = 0;
    csilk_mutex_unlock(&g_room_manager.mutex);
    csilk_mutex_destroy(&g_room_manager.mutex);
    g_room_manager_initialized = 0;
}

static void
ws_room_manager_init(void)
{
    if (g_room_manager_initialized) {
        return;
    }
    memset(&g_room_manager, 0, sizeof(ws_room_manager_t));
    csilk_mutex_init(&g_room_manager.mutex);
    g_room_manager_initialized = 1;
    atexit(ws_room_manager_cleanup);
}

static ws_room_t*
find_room(const char* name)
{
    for (int i = 0; i < g_room_manager.rooms_count; i++) {
        if (strcmp(g_room_manager.rooms[i]->room_name, name) == 0) {
            return g_room_manager.rooms[i];
        }
    }
    return NULL;
}

static void
on_room_message(csilk_mq_ctx_t* ctx)
{
    const char*    topic = csilk_mq_get_topic(ctx);
    size_t         payload_len;
    const uint8_t* payload = (const uint8_t*)csilk_mq_get_payload(ctx, &payload_len);

    if (!topic || strncmp(topic, "ws.room.", 8) != 0) {
        return;
    }

    const char* room_name = topic + 8;

    csilk_mutex_lock(&g_room_manager.mutex);
    ws_room_t*          room = find_room(room_name);
    ws_room_snapshot_t* snap = NULL;
    if (room) {
        snap = atomic_load(&room->snapshot);
    }
    csilk_mutex_unlock(&g_room_manager.mutex);

    /* Broadcast using COW snapshot without holding manager lock */
    if (snap && snap->clients) {
        for (int i = 0; i < snap->count; i++) {
            if (snap->clients[i]) {
                csilk_ws_send(snap->clients[i], payload, payload_len, 1);
            }
        }
    }
}

void
csilk_ws_join_room(csilk_ctx_t* c, const char* room_name)
{
    if (!c || !room_name) {
        return;
    }

    ws_room_manager_init();
    csilk_mutex_lock(&g_room_manager.mutex);

    ws_room_t* room = find_room(room_name);
    if (!room) {
        if (g_room_manager.rooms_count >= g_room_manager.rooms_capacity) {
            int new_cap =
                g_room_manager.rooms_capacity == 0 ? 16 : g_room_manager.rooms_capacity * 2;
            g_room_manager.rooms = realloc(g_room_manager.rooms, sizeof(ws_room_t*) * new_cap);
            g_room_manager.rooms_capacity = new_cap;
        }
        room = calloc(1, sizeof(ws_room_t));
        room->room_name = strdup(room_name);
        csilk_mutex_init(&room->mutex);

        ws_room_snapshot_t* empty_snap = calloc(1, sizeof(ws_room_snapshot_t));
        atomic_store(&room->snapshot, empty_snap);

        g_room_manager.rooms[g_room_manager.rooms_count++] = room;

        char topic[256];
        snprintf(topic, sizeof(topic), "ws.room.%s", room_name);
        csilk_mq_t* mq = csilk_ctx_get_mq(c);
        if (mq) {
            csilk_mq_subscribe(mq, topic, on_room_message);
        }
    }

    csilk_mutex_lock(&room->mutex);

    ws_room_snapshot_t* old_snap = atomic_load(&room->snapshot);
    int                 count = old_snap ? old_snap->count : 0;

    /* Check if already in room */
    for (int i = 0; i < count; i++) {
        if (old_snap->clients[i] == c) {
            csilk_mutex_unlock(&room->mutex);
            csilk_mutex_unlock(&g_room_manager.mutex);
            return;
        }
    }

    /* Allocate new COW snapshot */
    ws_room_snapshot_t* new_snap = calloc(1, sizeof(ws_room_snapshot_t));
    new_snap->count = count + 1;
    new_snap->clients = malloc(sizeof(csilk_ctx_t*) * new_snap->count);
    if (old_snap && old_snap->clients && count > 0) {
        memcpy(new_snap->clients, old_snap->clients, sizeof(csilk_ctx_t*) * count);
    }
    new_snap->clients[count] = c;

    atomic_store(&room->snapshot, new_snap);
    if (old_snap) {
        if (old_snap->clients) {
            free(old_snap->clients);
        }
        free(old_snap);
    }

    csilk_mutex_unlock(&room->mutex);
    csilk_mutex_unlock(&g_room_manager.mutex);
}

void
csilk_ws_leave_room(csilk_ctx_t* c, const char* room_name)
{
    if (!c || !room_name) {
        return;
    }

    ws_room_manager_init();
    csilk_mutex_lock(&g_room_manager.mutex);

    ws_room_t* room = find_room(room_name);
    if (room) {
        csilk_mutex_lock(&room->mutex);

        ws_room_snapshot_t* old_snap = atomic_load(&room->snapshot);
        if (old_snap && old_snap->count > 0) {
            int found_idx = -1;
            for (int i = 0; i < old_snap->count; i++) {
                if (old_snap->clients[i] == c) {
                    found_idx = i;
                    break;
                }
            }

            if (found_idx >= 0) {
                ws_room_snapshot_t* new_snap = calloc(1, sizeof(ws_room_snapshot_t));
                new_snap->count = old_snap->count - 1;
                if (new_snap->count > 0) {
                    new_snap->clients = malloc(sizeof(csilk_ctx_t*) * new_snap->count);
                    int write_idx = 0;
                    for (int i = 0; i < old_snap->count; i++) {
                        if (i != found_idx) {
                            new_snap->clients[write_idx++] = old_snap->clients[i];
                        }
                    }
                }
                atomic_store(&room->snapshot, new_snap);
                if (old_snap->clients) {
                    free(old_snap->clients);
                }
                free(old_snap);
            }
        }

        csilk_mutex_unlock(&room->mutex);
    }

    csilk_mutex_unlock(&g_room_manager.mutex);
}

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
