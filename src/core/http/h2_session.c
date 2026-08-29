/**
 * @file h2_session.c
 * @brief HTTP/2 session and stream management.
 */

#include "csilk/http/h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Multiplicative Hash & Table Resizing --- */

static inline uint32_t
_csilk_h2_stream_hash(int32_t stream_id, uint32_t mask)
{
    /* Knuth golden ratio 32-bit multiplicative hash for 31-bit stream IDs */
    return (uint32_t)(((uint32_t)stream_id * 2654435761u) & mask);
}

static inline void
_csilk_h2_stream_map_ensure_init(csilk_h2_stream_map_t* map)
{
    if (!map->buckets) {
        map->capacity = CSILK_H2_INLINE_BUCKETS;
        map->mask = CSILK_H2_INLINE_BUCKETS - 1;
        map->count = 0;
        map->buckets = map->inline_buckets;
        map->free_list = NULL;
        map->pool_count = 0;
        map->pool_max = CSILK_H2_STREAM_POOL_MAX;
        memset(map->inline_buckets, 0, sizeof(map->inline_buckets));
    }
}

static void
_csilk_h2_stream_map_resize(csilk_h2_stream_map_t* map)
{
    uint32_t old_cap = map->capacity;
    uint32_t new_cap = old_cap * 2;
    if (new_cap < old_cap || new_cap > 65536) {
        return; /* Bounded max bucket capacity */
    }

    csilk_ctx_t** new_buckets = calloc(new_cap, sizeof(csilk_ctx_t*));
    if (!new_buckets) {
        return; /* Keep running on existing capacity on OOM */
    }

    uint32_t new_mask = new_cap - 1;
    for (uint32_t i = 0; i < old_cap; i++) {
        csilk_ctx_t* curr = map->buckets[i];
        while (curr) {
            csilk_ctx_t* next = curr->next_stream;
            uint32_t     new_idx = _csilk_h2_stream_hash(curr->stream_id, new_mask);
            curr->next_stream = new_buckets[new_idx];
            new_buckets[new_idx] = curr;
            curr = next;
        }
    }

    if (map->buckets != map->inline_buckets) {
        free(map->buckets);
    }

    map->buckets = new_buckets;
    map->capacity = new_cap;
    map->mask = new_mask;
}

/* --- Stream reference counting & lifecycle --- */

static void
_csilk_stream_destroy_physically(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    csilk_client_t*        client = c->h2_stream_owner;
    csilk_h2_stream_map_t* map = client ? &client->h2_stream_map : NULL;

    /* Clean up any request/response bodies, storage items, defers */
    csilk_ctx_cleanup(c);

    if (map && map->pool_count < map->pool_max) {
        c->stream_state = CSILK_STREAM_STATE_RECYCLED;
        if (c->arena) {
            csilk_arena_reset(c->arena);
        }
        c->next_stream = map->free_list;
        map->free_list = c;
        map->pool_count++;
    } else {
        c->stream_state = CSILK_STREAM_STATE_CLOSED;
        if (c->arena) {
            csilk_arena_free(c->arena);
            c->arena = NULL;
        }
        free(c);
    }
}

static void
_csilk_stream_destroy_dispatch_cb(void* arg)
{
    csilk_ctx_t* c = (csilk_ctx_t*)arg;
    if (!c) {
        return;
    }
    _csilk_stream_destroy_physically(c);
}

void
_csilk_stream_ref(csilk_ctx_t* c)
{
    if (c && c->h2_stream_owner) {
        atomic_fetch_add_explicit(&c->stream_ref, 1, memory_order_relaxed);
    }
}

void
_csilk_stream_unref(csilk_ctx_t* c)
{
    if (!c || !c->h2_stream_owner) {
        return;
    }
    if (atomic_fetch_sub_explicit(&c->stream_ref, 1, memory_order_acq_rel) > 1) {
        return;
    }

    /* stream_ref reached 0 — perform physical teardown / recycling */
    csilk_client_t* client = c->h2_stream_owner;
    if (client && client->owner_pool && !_csilk_is_owner_worker_thread(client->owner_pool)) {
        /* If not on owner worker thread, dispatch physical cleanup to owner worker */
        csilk_dispatch(c, _csilk_stream_destroy_dispatch_cb, c);
        return;
    }

    _csilk_stream_destroy_physically(c);
}

/* --- Stream lookup/creation --- */

csilk_ctx_t*
csilk_h2_get_stream(csilk_client_t* client, int32_t stream_id)
{
    if (!client) {
        return NULL;
    }
    csilk_h2_stream_map_t* map = &client->h2_stream_map;
    if (!map->buckets || map->count == 0) {
        return NULL;
    }
    uint32_t     idx = _csilk_h2_stream_hash(stream_id, map->mask);
    csilk_ctx_t* curr = map->buckets[idx];
    while (curr) {
        if (curr->stream_id == stream_id) {
            return curr;
        }
        curr = curr->next_stream;
    }
    return NULL;
}

csilk_ctx_t*
csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id)
{
    if (!client) {
        return NULL;
    }

    csilk_h2_stream_map_t* map = &client->h2_stream_map;
    _csilk_h2_stream_map_ensure_init(map);

    uint32_t     idx = _csilk_h2_stream_hash(stream_id, map->mask);
    csilk_ctx_t* curr = map->buckets[idx];
    while (curr) {
        if (curr->stream_id == stream_id) {
            return curr;
        }
        curr = curr->next_stream;
    }

    /* Auto-resize when load factor threshold reached */
    if (map->count >= map->capacity) {
        _csilk_h2_stream_map_resize(map);
        idx = _csilk_h2_stream_hash(stream_id, map->mask);
    }

    csilk_ctx_t*   ctx = NULL;
    csilk_arena_t* arena = NULL;

    /* Acquire from pool if available */
    if (map->free_list) {
        ctx = map->free_list;
        map->free_list = ctx->next_stream;
        map->pool_count--;

        arena = ctx->arena;
        /* Reset arena to clean initial state (keeps 4KB head chunk, 0 syscall) */
        csilk_arena_reset(arena);
        ctx->stream_id = stream_id;
        ctx->request_seq++;
        ctx->stream_gen++;
        atomic_store_explicit(&ctx->stream_ref, 1, memory_order_relaxed);
        ctx->stream_state = CSILK_STREAM_STATE_ACTIVE;
        ctx->stream_closed = 0;
        ctx->headers_received = 0;
        ctx->end_stream_received = 0;
        ctx->request_dispatched = 0;
        ctx->request_cancelled = 0;
        ctx->handler_index = -1;
        ctx->aborted = 0;
        ctx->panicked = 0;
        ctx->is_async = 0;
        ctx->response_started = 0;
        ctx->params_count = 0;
        ctx->defer_head = NULL;
        ctx->storage_head = NULL;
        ctx->current_handler = NULL;
    } else {
        /* Allocate new context and arena */
        ctx = malloc(sizeof(csilk_ctx_t));
        if (!ctx) {
            return NULL;
        }
        arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        if (client->server && client->server->config.enable_arena_alignment) {
            csilk_arena_set_alignment(arena, 1);
        }
        _csilk_stream_ctx_init(ctx, client, stream_id);
        ctx->arena = arena;
    }

    /* Insert into bucket chain head */
    ctx->next_stream = map->buckets[idx];
    map->buckets[idx] = ctx;
    map->count++;

    return ctx;
}

/**
 * @brief Remove and free a stream context from the client's stream map by stream ID.
 * @param[in] client    Client whose stream is being removed.
 * @param[in] stream_id HTTP/2 stream ID to close.
 * @return 0 on success, -1 if not found.
 */
int
csilk_h2_remove_stream(csilk_client_t* client, int32_t stream_id)
{
    if (!client) {
        return -1;
    }

    csilk_h2_stream_map_t* map = &client->h2_stream_map;
    if (!map->buckets || map->count == 0) {
        return -1;
    }

    uint32_t      idx = _csilk_h2_stream_hash(stream_id, map->mask);
    csilk_ctx_t** curr = &map->buckets[idx];
    while (*curr) {
        if ((*curr)->stream_id == stream_id) {
            csilk_ctx_t* found = *curr;
            if (found->h2_stream_owner != client) {
                return -1;
            }
            *curr = found->next_stream;
            found->next_stream = NULL;
            found->stream_closed = 1;
            found->stream_state = CSILK_STREAM_STATE_CLOSED;
            map->count--;

            /* Release active map reference (defers destruction/recycling if async op holds reference) */
            _csilk_stream_unref(found);
            return 0;
        }
        curr = &((*curr)->next_stream);
    }

    return -1;
}

/**
 * @brief Free all HTTP/2 streams associated with a client connection.
 * @param[in] client Client whose stream map is torn down.
 */
void
csilk_h2_free_streams(csilk_client_t* client)
{
    if (!client) {
        return;
    }

    csilk_h2_stream_map_t* map = &client->h2_stream_map;
    if (!map->buckets) {
        return;
    }

    /* Unlink and unref all active streams */
    for (uint32_t i = 0; i < map->capacity; i++) {
        csilk_ctx_t* curr = map->buckets[i];
        while (curr) {
            csilk_ctx_t* next = curr->next_stream;
            curr->next_stream = NULL;
            curr->stream_closed = 1;
            curr->stream_state = CSILK_STREAM_STATE_CLOSED;
            _csilk_stream_unref(curr);
            curr = next;
        }
        map->buckets[i] = NULL;
    }
    map->count = 0;

    /* Free all pooled idle streams */
    csilk_ctx_t* pool_curr = map->free_list;
    while (pool_curr) {
        csilk_ctx_t* next = pool_curr->next_stream;
        if (pool_curr->arena) {
            csilk_arena_free(pool_curr->arena);
            pool_curr->arena = NULL;
        }
        free(pool_curr);
        pool_curr = next;
    }
    map->free_list = NULL;
    map->pool_count = 0;

    if (map->buckets != map->inline_buckets) {
        free(map->buckets);
    }

    map->buckets = map->inline_buckets;
    map->capacity = CSILK_H2_INLINE_BUCKETS;
    map->mask = CSILK_H2_INLINE_BUCKETS - 1;
    map->count = 0;
    memset(map->inline_buckets, 0, sizeof(map->inline_buckets));
}

/* --- Session initialization --- */

int
csilk_h2_init_session(csilk_client_t* client)
{
    nghttp2_session_callbacks* callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return -1;
    }

    extern int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_header_callback(nghttp2_session*,
                                  const nghttp2_frame*,
                                  const uint8_t*,
                                  size_t,
                                  const uint8_t*,
                                  size_t,
                                  uint8_t,
                                  void*);
    extern int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_data_chunk_recv_callback(
        nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void*);
    extern int     on_stream_close_callback(nghttp2_session*, int32_t, uint32_t, void*);
    extern ssize_t send_callback(nghttp2_session*, const uint8_t*, size_t, int, void*);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                              on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);

    if (nghttp2_session_server_new(&client->h2_session, callbacks, client) != 0) {
        nghttp2_session_callbacks_del(callbacks);
        return -1;
    }

    nghttp2_session_callbacks_del(callbacks);

    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };

    if (nghttp2_submit_settings(client->h2_session, NGHTTP2_FLAG_NONE, iv, 1) != 0) {
        return -1;
    }

    nghttp2_session_send(client->h2_session);

    return 0;
}

/* --- Data processing --- */

int
csilk_h2_process_data(csilk_client_t* client, const uint8_t* data, size_t len)
{
    ssize_t rv = nghttp2_session_mem_recv(client->h2_session, data, len);
    if (rv < 0) {
        return -1;
    }

    if (nghttp2_session_send(client->h2_session) != 0) {
        return -1;
    }

    return 0;
}
