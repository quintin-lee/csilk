/**
 * @file ws_room.c
 * @brief WebSocket room management and broadcast based on MQ with Copy-On-Write (COW).
 *
 * Implements a high-concurrency room broadcasting system for WebSockets using COW
 * snapshots to avoid lock holding during network writes.
 *
 * The room manager is allocated on the csilk_server_t instance (ws_room_mgr
 * field) and freed by csilk_server_free(). This eliminates the previous
 * process-wide global + atexit pattern, giving each server its own
 * independent room state.
 *
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#ifdef CSILK_HAS_MQ
#include "../messaging/mq_internal.h"
#endif
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"

static void _csilk_ws_room_mgr_free(csilk_server_t* server);

typedef struct {
    csilk_ctx_t** clients;
    int           count;
    _Atomic int   refs; /**< Strong refs held: one by the owning room + N by in-flight readers. */
} ws_room_snapshot_t;

/** @brief Take a strong reference to a COW snapshot. @return 1 on success. */
static int
ws_room_snap_acquire(ws_room_snapshot_t* snap)
{
    if (!snap) {
        return 0;
    }
    atomic_fetch_add_explicit(&snap->refs, 1, memory_order_acquire);
    return 1;
}

/** @brief Drop one strong reference; frees the snapshot when the last ref goes away. */
static void
ws_room_snap_release(ws_room_snapshot_t* snap)
{
    if (!snap) {
        return;
    }
    int prev = atomic_fetch_sub_explicit(&snap->refs, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_seq_cst);
    if (prev == 1) {
        if (snap->clients) {
            free(snap->clients);
        }
        free(snap);
    }
}

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

/** @brief Tear down a room manager and free all rooms, snapshots, names. */
static void
ws_room_manager_free(ws_room_manager_t* mgr)
{
    if (!mgr) {
        return;
    }
    csilk_mutex_lock(&mgr->mutex);
    for (int i = 0; i < mgr->rooms_count; i++) {
        ws_room_t* room = mgr->rooms[i];
        if (room) {
            ws_room_snapshot_t* snap = atomic_load(&room->snapshot);
            /* At teardown all in-flight readers have completed, so the room's
             * baseline ref is the last strong reference and release frees it. */
            ws_room_snap_release(snap);
            free(room->room_name);
            csilk_mutex_destroy(&room->mutex);
            free(room);
        }
    }
    free(mgr->rooms);
    csilk_mutex_unlock(&mgr->mutex);
    csilk_mutex_destroy(&mgr->mutex);
    free(mgr);
}

/** @brief Lazy-init the room manager on the server instance. */
static ws_room_manager_t*
ws_room_manager_get(csilk_server_t* server)
{
    if (!server) {
        return NULL;
    }
    ws_room_manager_t* mgr = (ws_room_manager_t*)server->ws_room_mgr;
    if (!mgr) {
        mgr = calloc(1, sizeof(ws_room_manager_t));
        if (!mgr) {
            return NULL;
        }
        csilk_mutex_init(&mgr->mutex);
        server->ws_room_mgr = mgr;
        _csilk_server_register_teardown(server, _csilk_ws_room_mgr_free);
    }
    return mgr;
}

/** @brief Get the room manager for the server owning @p c. */
static ws_room_manager_t*
ws_room_manager_from_ctx(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    return ws_room_manager_get(csilk_ctx_get_server(c));
}

/** @brief Linear search of the manager's rooms for one matching @p name.
 *
 *  Caller must hold mgr->mutex. */
static ws_room_t*
find_room_locked(ws_room_manager_t* mgr, const char* name)
{
    for (int i = 0; i < mgr->rooms_count; i++) {
        if (strcmp(mgr->rooms[i]->room_name, name) == 0) {
            return mgr->rooms[i];
        }
    }
    return NULL;
}

typedef struct {
    csilk_client_t* client;
    uint64_t        gen;
    uint8_t*        payload;
    size_t          len;
} ws_room_send_job_t;

static void
_ws_room_send_dispatch_cb(void* arg)
{
    ws_room_send_job_t* job = (ws_room_send_job_t*)arg;
    if (!job) {
        return;
    }
    if (job->client && job->client->generation == job->gen &&
        csilk_conn_get_state(job->client) == CSILK_CONN_STREAMING) {
        csilk_ws_send(&job->client->ctx, job->payload, job->len, 1);
    }
    if (job->client) {
        csilk_client_unref(job->client);
    }
    free(job->payload);
    free(job);
}

typedef struct {
    csilk_ctx_t* c;
    char**       rooms;
    int          count;
    int          cap;
} ws_room_membership_t;

/** @brief Remove @p c from @p room_name's snapshot. Caller must hold mgr->mutex. */
static void
_ws_room_remove_locked(ws_room_manager_t* mgr, csilk_ctx_t* c, const char* room_name)
{
    ws_room_t* room = find_room_locked(mgr, room_name);
    if (!room) {
        return;
    }
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
            if (new_snap) {
                new_snap->count = old_snap->count - 1;
                if (new_snap->count > 0) {
                    new_snap->clients = malloc(sizeof(csilk_ctx_t*) * (size_t)new_snap->count);
                    if (!new_snap->clients) {
                        free(new_snap);
                        new_snap = NULL;
                    } else {
                        int write_idx = 0;
                        for (int i = 0; i < old_snap->count; i++) {
                            if (i != found_idx) {
                                new_snap->clients[write_idx++] = old_snap->clients[i];
                            }
                        }
                    }
                }
                if (new_snap) {
                    atomic_init(&new_snap->refs, 1);
                    atomic_store(&room->snapshot, new_snap);
                    ws_room_snap_release(old_snap);
                }
            }
        }
    }
    csilk_mutex_unlock(&room->mutex);
}

static void
ws_room_membership_cleanup(void* data)
{
    ws_room_membership_t* mem = (ws_room_membership_t*)data;
    if (!mem) {
        return;
    }
    csilk_server_t*    server = csilk_ctx_get_server(mem->c);
    ws_room_manager_t* mgr = server ? (ws_room_manager_t*)server->ws_room_mgr : NULL;
    if (mgr) {
        csilk_mutex_lock(&mgr->mutex);
        for (int i = 0; i < mem->count; i++) {
            if (mem->rooms[i]) {
                _ws_room_remove_locked(mgr, mem->c, mem->rooms[i]);
            }
        }
        csilk_mutex_unlock(&mgr->mutex);
    }
    for (int i = 0; i < mem->count; i++) {
        free(mem->rooms[i]);
    }
    free(mem->rooms);
    free(mem);
}

static void
ws_room_track_join(csilk_ctx_t* c, const char* room_name)
{
    ws_room_membership_t* mem = (ws_room_membership_t*)csilk_get(c, "_ws_rooms");
    if (!mem) {
        mem = calloc(1, sizeof(ws_room_membership_t));
        if (!mem) {
            return;
        }
        mem->c = c;
        csilk_set_ex(c, "_ws_rooms", mem, ws_room_membership_cleanup);
    }
    for (int i = 0; i < mem->count; i++) {
        if (mem->rooms[i] && strcmp(mem->rooms[i], room_name) == 0) {
            return;
        }
    }
    if (mem->count >= mem->cap) {
        int    nc = mem->cap == 0 ? 4 : mem->cap * 2;
        char** nr = realloc(mem->rooms, sizeof(char*) * (size_t)nc);
        if (!nr) {
            return;
        }
        mem->rooms = nr;
        mem->cap = nc;
    }
    char* dup = strdup(room_name);
    if (dup) {
        mem->rooms[mem->count++] = dup;
    }
}

static void
ws_room_track_leave(csilk_ctx_t* c, const char* room_name)
{
    ws_room_membership_t* mem = (ws_room_membership_t*)csilk_get(c, "_ws_rooms");
    if (!mem) {
        return;
    }
    for (int i = 0; i < mem->count; i++) {
        if (mem->rooms[i] && strcmp(mem->rooms[i], room_name) == 0) {
            free(mem->rooms[i]);
            mem->rooms[i] = mem->rooms[--mem->count];
            break;
        }
    }
}

static void
ws_room_deliver(ws_room_manager_t* mgr,
                const char*        room_name,
                const uint8_t*     payload,
                size_t             payload_len)
{
    if (!mgr || !room_name) {
        return;
    }

    csilk_mutex_lock(&mgr->mutex);
    ws_room_t*          room = find_room_locked(mgr, room_name);
    ws_room_snapshot_t* snap = NULL;
    if (room) {
        snap = atomic_load(&room->snapshot);
        /* Take a strong ref while still under the manager lock so a
         * concurrent join/leave cannot drop the last reference to this
         * snapshot before we finish walking it. */
        ws_room_snap_acquire(snap);
    }
    csilk_mutex_unlock(&mgr->mutex);

    /* Broadcast using the COW snapshot without holding the manager lock. */
    if (snap && snap->clients) {
        for (int i = 0; i < snap->count; i++) {
            csilk_ctx_t* target = snap->clients[i];
            if (!target) {
                continue;
            }
            csilk_client_t* cl = (csilk_client_t*)_csilk_get_internal_client(target);
            if (!cl) {
                /* Mock/unit-test context without a live worker connection. */
                csilk_ws_send(target, payload, payload_len, 1);
                continue;
            }
            if (csilk_client_ref(cl) != 0) {
                continue;
            }
            ws_room_send_job_t* job = malloc(sizeof(ws_room_send_job_t));
            uint8_t*            copy = payload_len > 0 ? malloc(payload_len) : NULL;
            if (!job || (payload_len > 0 && !copy)) {
                free(copy);
                free(job);
                csilk_client_unref(cl);
                continue;
            }
            if (payload_len > 0) {
                memcpy(copy, payload, payload_len);
            }
            job->client = cl;
            job->gen = cl->generation;
            job->payload = copy;
            job->len = payload_len;
            if (_csilk_dispatch_try(target, _ws_room_send_dispatch_cb, job) < 0) {
                free(job->payload);
                free(job);
                csilk_client_unref(cl);
            }
        }
    }
    /* Release our reader ref; the snapshot is freed only when the owning
     * room's baseline ref and every other in-flight reader have released. */
    ws_room_snap_release(snap);
}

#ifdef CSILK_HAS_MQ
/** @brief MQ callback: broadcast a "ws.room.<name>" payload to that room's snapshot. */
static void
on_room_message(csilk_mq_ctx_t* ctx)
{
    const char*    topic = csilk_mq_get_topic(ctx);
    size_t         payload_len;
    const uint8_t* payload = (const uint8_t*)csilk_mq_get_payload(ctx, &payload_len);

    if (!topic || strncmp(topic, "ws.room.", 8) != 0) {
        return;
    }

    const char*        room_name = topic + 8;
    csilk_server_t*    server = (ctx && ctx->mq) ? ctx->mq->server : NULL;
    ws_room_manager_t* mgr = server ? (ws_room_manager_t*)server->ws_room_mgr : NULL;
    ws_room_deliver(mgr, room_name, payload, payload_len);
}
#endif

/**
 * @brief Free the WebSocket room manager owned by a server instance.
 *
 * Registered via _csilk_server_register_teardown() to tear down per-server room state.
 */
static void
_csilk_ws_room_mgr_free(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    ws_room_manager_free((ws_room_manager_t*)server->ws_room_mgr);
    server->ws_room_mgr = NULL;
}

/**
 * @brief Add a context (client connection) to a named WebSocket room.
 *
 * Lazily initializes the room manager on the server instance and creates the
 * room on first use, subscribing the connection's MQ to the "ws.room.<name>"
 * topic. Membership is tracked via a Copy-On-Write atomic snapshot so
 * broadcasts avoid holding the room lock. Re-joining an already-joined room
 * is a no-op.
 *
 * @param[in,out] c          The client context to add.
 * @param[in]     room_name  Name of the room to join.
 */
void
csilk_ws_join_room(csilk_ctx_t* c, const char* room_name)
{
    if (!c || !room_name) {
        return;
    }

    ws_room_manager_t* mgr = ws_room_manager_from_ctx(c);
    if (!mgr) {
        return;
    }
    csilk_mutex_lock(&mgr->mutex);

    ws_room_t* room = find_room_locked(mgr, room_name);
    if (!room) {
        if (mgr->rooms_count >= mgr->rooms_capacity) {
            int         new_cap = mgr->rooms_capacity == 0 ? 16 : mgr->rooms_capacity * 2;
            ws_room_t** next_rooms = realloc(mgr->rooms, sizeof(ws_room_t*) * (size_t)new_cap);
            if (!next_rooms) {
                csilk_mutex_unlock(&mgr->mutex);
                return;
            }
            mgr->rooms = next_rooms;
            mgr->rooms_capacity = new_cap;
        }
        room = calloc(1, sizeof(ws_room_t));
        char*               rname = strdup(room_name);
        ws_room_snapshot_t* empty_snap = calloc(1, sizeof(ws_room_snapshot_t));
        if (!room || !rname || !empty_snap) {
            free(empty_snap);
            free(rname);
            free(room);
            csilk_mutex_unlock(&mgr->mutex);
            return;
        }
        room->room_name = rname;
        csilk_mutex_init(&room->mutex);

        /* Room holds the baseline strong ref to its snapshot (refs starts at 1). */
        atomic_init(&empty_snap->refs, 1);
        atomic_init(&room->snapshot, empty_snap);

        mgr->rooms[mgr->rooms_count++] = room;

#ifdef CSILK_HAS_MQ
        char topic[256];
        snprintf(topic, sizeof(topic), "ws.room.%s", room_name);
        csilk_mq_t* mq = csilk_ctx_get_mq(c);
        if (mq) {
            csilk_mq_subscribe(mq, topic, on_room_message);
        }
#endif
    }

    csilk_mutex_lock(&room->mutex);

    ws_room_snapshot_t* old_snap = atomic_load(&room->snapshot);
    /* The room always holds a baseline strong ref to its current snapshot.
     * old_snap == NULL only at room creation, when the empty snapshot was
     * published with refs==1 and the room owns that single ref. */
    int count = old_snap ? old_snap->count : 0;

    /* Check if already in room */
    for (int i = 0; i < count; i++) {
        if (old_snap->clients[i] == c) {
            csilk_mutex_unlock(&room->mutex);
            csilk_mutex_unlock(&mgr->mutex);
            return;
        }
    }

    /* Allocate new COW snapshot; it inherits the room's baseline ref. */
    ws_room_snapshot_t* new_snap = calloc(1, sizeof(ws_room_snapshot_t));
    if (!new_snap) {
        csilk_mutex_unlock(&room->mutex);
        csilk_mutex_unlock(&mgr->mutex);
        return;
    }
    new_snap->count = count + 1;
    new_snap->clients = malloc(sizeof(csilk_ctx_t*) * (size_t)new_snap->count);
    if (!new_snap->clients) {
        free(new_snap);
        csilk_mutex_unlock(&room->mutex);
        csilk_mutex_unlock(&mgr->mutex);
        return;
    }
    atomic_init(&new_snap->refs, 1);
    if (old_snap && old_snap->clients && count > 0) {
        memcpy(new_snap->clients, old_snap->clients, sizeof(csilk_ctx_t*) * (size_t)count);
    }
    new_snap->clients[count] = c;

    atomic_store(&room->snapshot, new_snap);
    /* Release the baseline ref the room held on old_snap. In-flight readers
     * (on_room_message) that already acquired old_snap keep their own ref,
     * so it is only freed once every reader releases. */
    ws_room_snap_release(old_snap);

    csilk_mutex_unlock(&room->mutex);
    csilk_mutex_unlock(&mgr->mutex);
    ws_room_track_join(c, room_name);
}

/**
 * @brief Remove a context from a named WebSocket room.
 *
 * Rebuilds the room's COW snapshot without @p c and publishes nothing. If the
 * room is not found the call is a no-op.
 *
 * @param[in,out] c          The client context to remove.
 * @param[in]     room_name  Name of the room to leave.
 */
void
csilk_ws_leave_room(csilk_ctx_t* c, const char* room_name)
{
    if (!c || !room_name) {
        return;
    }

    ws_room_manager_t* mgr = ws_room_manager_from_ctx(c);
    if (!mgr) {
        return;
    }
    csilk_mutex_lock(&mgr->mutex);
    _ws_room_remove_locked(mgr, c, room_name);
    csilk_mutex_unlock(&mgr->mutex);
    ws_room_track_leave(c, room_name);
}

/**
 * @brief Publish a message to every member of a WebSocket room.
 *
 * Publishes @p message to the MQ topic "ws.room.<room_name>"; each member's
 * subscription callback (on_room_message) replays it to that client. Requires
 * a valid MQ on @p c.
 *
 * @param[in,out] c          The sending client context (provides the MQ).
 * @param[in]     room_name  Name of the room to broadcast to.
 * @param[in]     message    NUL-terminated message text to publish.
 */
void
csilk_ws_broadcast_room(csilk_ctx_t* c, const char* room_name, const char* message)
{
    if (!c || !room_name || !message) {
        return;
    }
#ifdef CSILK_HAS_MQ
    csilk_mq_t* mq = csilk_ctx_get_mq(c);
    if (!mq) {
        return;
    }

    char topic[256];
    snprintf(topic, sizeof(topic), "ws.room.%s", room_name);
    csilk_mq_publish(mq, topic, message, strlen(message));
#else
    ws_room_manager_t* mgr = ws_room_manager_from_ctx(c);
    ws_room_deliver(mgr, room_name, (const uint8_t*)message, strlen(message));
#endif
}
