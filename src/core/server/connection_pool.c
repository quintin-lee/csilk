/**
 * @file connection_pool.c
 * @brief Connection pool, arena pool, and read buffer pool management.
 */

#include <openssl/ssl.h>
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "../http/h2.h"

/* --- Buffer allocation --- */

/** @brief Forward declaration for pool_get_read_buf. */
static void pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf);

/** @brief I/O buffer allocation callback — allocates a receive buffer.
 *
 * Allocates a buffer of the suggested size using malloc. The buffer is freed
 * by the I/O backend after the read callback is invoked (libuv path).
 *
 * @param handle          The I/O handle that will read into the buffer.
 * @param suggested_size  Recommended buffer size from the I/O backend.
 * @param buf             [out] Pointer to the csilk_io_buf_t to populate.
 */
void
alloc_buffer(csilk_io_handle_t* handle, size_t suggested_size, csilk_io_buf_t* buf)
{
    worker_pool_t* wp = NULL;
    if (handle && handle->data) {
        csilk_client_t* client = (csilk_client_t*)handle->data;
        wp = client->owner_pool;
    }
    pool_get_read_buf(wp, suggested_size, buf);
}

/* --- Connection pool (per-worker, lock-free) --- */

/** @brief Get a client connection object from the worker-local free pool or
 *  allocate a new one.
 *
 * In multi-worker mode, each worker thread has its own pool with no shared
 * state — pool_get is a pure thread-local O(1) operation with zero locking.
 *
 * @param wp The worker pool (must not be NULL).
 * @return A csilk_client_t ready for use, or NULL on allocation failure.
 */
csilk_client_t*
pool_get(worker_pool_t* wp)
{
    csilk_client_t* client;
    if (wp && wp->client_pool_count > 0) {
        client = wp->client_pool[--wp->client_pool_count];
    } else {
        client = calloc(1, sizeof(csilk_client_t));
    }
    if (client) {
        uint8_t gen = (uint8_t)((client->generation + 1) & 0xFF);
        if (gen == 0) {
            gen = 1;
        }
        csilk_conn_set_state(client, CSILK_CONN_INIT);
        client->generation = gen;
        atomic_store(&client->ref_count, 0);
        atomic_store(&client->pending_io, 0);
#ifdef CSILK_USE_URING
        client->handle.generation = gen;
        client->timer.generation = gen;
        client->read_timer.generation = gen;
        client->write_timer.generation = gen;
        client->request_timer.generation = gen;
#endif
        client->ctx.conn_closed = 0;
        client->ctx.file_fd = -1;
    }
    return client;
}

/** @brief Reset only the mutable/hot request and connection fields during recycling.
 *
 * Avoids expensive full-struct memset (which blows CPU caches and clears
 * immutable handle memory) while preserving generation counters and pre-allocated
 * I/O buffers.
 *
 * @param client The client connection to reset.
 */
static void
reset_hot_state(csilk_client_t* client)
{
    if (client->ssl) {
        SSL_free(client->ssl);
        client->ssl = NULL;
        client->read_bio = NULL;
        client->write_bio = NULL;
    }
    if (client->h2_session) {
        nghttp2_session_del(client->h2_session);
        client->h2_session = NULL;
    }
    csilk_h2_free_streams(client);

    /* Connection lifecycle and parser flags */
    csilk_conn_set_state(client, CSILK_CONN_INIT);
    atomic_store(&client->ref_count, 0);
    atomic_store(&client->pending_io, 0);
    client->read_paused = 0;
    client->read_active = 0;
    client->keep_alive = 0;
    client->pending_write_bytes = 0;
    client->protocol = CSILK_PROTO_HTTP1;
    client->total_header_size = 0;
    client->header_count = 0;
    client->current_url.data = NULL;
    client->current_url.len = 0;
    client->current_header_field.data = NULL;
    client->current_header_field.len = 0;
    client->current_header_value.data = NULL;
    client->current_header_value.len = 0;
    client->next = NULL;
    client->prev = NULL;
    client->server = NULL;
    client->owner_pool = NULL;

    /* Context mutable state */
    client->ctx.handler_index = -1;
    client->ctx.handlers = NULL;
    client->ctx.handler_count = 0;
    client->ctx.aborted = 0;
    client->ctx.panicked = 0;
    client->ctx.defer_head = NULL;
    client->ctx.params_count = 0;
    client->ctx.is_websocket = 0;
    client->ctx.is_sse = 0;
    client->ctx.is_async = 0;
    client->ctx.response_started = 0;
    client->ctx.write_paused = 0;
    client->ctx.on_drain = NULL;
    client->ctx.on_drain_data = NULL;
    client->ctx.file_fd = -1;
    client->ctx.file_offset = 0;
    client->ctx.file_size = 0;
    client->ctx.storage_head = NULL;
    client->ctx.stream_id = 0;
    client->ctx.next_stream = NULL;
    client->ctx.conn_closed = 0;
    client->ctx.on_ws_message = NULL;
    client->ctx.on_ws_send = NULL;
    client->ctx.read_buffers = client->ctx.read_buffers_embedded;
    client->ctx.read_buffers_count = 0;
    client->ctx.read_buffers_capacity = 16;
    client->ctx.read_buf_sizes = client->ctx.read_buf_sizes_embedded;
    memset(client->ctx.request_id, 0, sizeof(client->ctx.request_id));
}

/** @brief Return a client to the worker-local free pool for reuse.
 *
 * SSL and H2 sessions are cleaned before returning. Only hot mutable state is
 * reset via reset_hot_state() to avoid cache eviction from full struct zeroing.
 * If the pool has room, the client is saved for reuse; otherwise freed. No lock
 * — only the owning event-loop thread accesses this.
 *
 * @param wp     The worker pool (derived from client->owner_pool).
 * @param client The client to return (must not be used after this call).
 */
void
pool_put(worker_pool_t* wp, csilk_client_t* client)
{
    if (!client) {
        return;
    }
    reset_hot_state(client);
    if (wp && wp->client_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->client_pool[wp->client_pool_count++] = client;
    } else {
#ifdef CSILK_USE_URING
        if (client->read_buf) {
            pool_put_read_buf(wp, (char*)client->read_buf, CSILK_READ_BUF_64KB);
            client->read_buf = NULL;
        }
#endif
        free(client);
    }
}

/* --- Arena pool --- */

/** @brief Get a pre-allocated arena from the worker-local arena pool.
 *
 * Pops a pre-allocated arena from the pool. If the pool is empty, falls back
 * to creating a new arena on the fly. Pre-allocated arenas already have their
 * first chunk ready, so the hot path avoids aligned_alloc entirely.
 *
 * @param wp The worker pool (must not be NULL).
 * @return A csilk_arena_t ready for use, or NULL on allocation failure.
 */
csilk_arena_t*
pool_get_arena(worker_pool_t* wp)
{
    if (!wp) {
        return csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    }
    csilk_arena_t* arena;
    if (wp->arena_pool_count > 0) {
        arena = wp->arena_pool[--wp->arena_pool_count];
    } else {
        arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        if (arena && wp->server->config.enable_arena_alignment) {
            csilk_arena_set_alignment(arena, 1);
        }
    }
    return arena;
}

/** @brief Return an arena to the worker-local arena pool for reuse.
 *
 * Resets the arena (zero-clear, no system calls) and pushes it back into
 * the pool. If the pool is full, frees the arena normally.
 *
 * @param wp    The worker pool.
 * @param arena The arena to return (must not be used after this call).
 */
void
pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena)
{
    if (!arena) {
        return;
    }
    csilk_arena_reset(arena);
    if (wp && wp->arena_pool_count < CSILK_CLIENT_POOL_SIZE) {
        wp->arena_pool[wp->arena_pool_count++] = arena;
    } else {
        csilk_arena_free(arena);
    }
}

/** @brief Pre-populate the worker-local arena pool with ready-to-use arenas.
 *
 * Each pre-allocated arena has its first chunk already allocated and reset,
 * so the hot accept path (on_new_connection) performs zero aligned_alloc
 * calls — the arena is popped from the pool and used immediately.
 *
 * Alignment is set according to the server config at pre-alloc time so it
 * does not need to be repeated per-connection.
 *
 * @param wp The worker pool to initialise.
 */
void
_csilk_worker_init_arena_pool(worker_pool_t* wp)
{
    int align = wp->server->config.enable_arena_alignment;
    for (int i = 0; i < CSILK_CLIENT_POOL_SIZE; i++) {
        csilk_arena_t* a = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        if (!a) {
            break;
        }
        if (align) {
            csilk_arena_set_alignment(a, 1);
        }
        /* Pre-allocate the first chunk so csilk_arena_alloc in the hot
         * path always hits the fast (bump) path. */
        void* p = csilk_arena_alloc(a, 1);
        if (!p) {
            csilk_arena_free(a);
            break;
        }
        csilk_arena_reset(a);
        wp->arena_pool[wp->arena_pool_count++] = a;
    }
}

/* --- Read buffer pool (three-tier: 4KB / 16KB / 64KB) --- */

/** @brief Select the tier index for a given buffer size.
 *
 * Returns the smallest tier whose capacity is >= suggested_size.
 * Falls back to the largest tier if no smaller tier fits.
 *
 * @param suggested_size Requested buffer size in bytes.
 * @return Tier index (0 = 4KB, 1 = 16KB, 2 = 64KB).
 */
static int
read_buf_tier_index(size_t suggested_size)
{
    if (suggested_size <= CSILK_READ_BUF_4KB) {
        return 0;
    }
    if (suggested_size <= CSILK_READ_BUF_16KB) {
        return 1;
    }
    return 2;
}

/** @brief Get a read buffer from the worker-local pool.
 *
 * Pops a buffer from the tier matching suggested_size. If the tier is empty,
 * falls back to malloc. The buffer is returned via @p buf.
 *
 * @param wp             The worker pool (must not be NULL).
 * @param suggested_size Requested buffer size.
 * @param buf            [out] Populated with base pointer and length.
 */
static void
pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf)
{
    int    tier = read_buf_tier_index(suggested_size);
    size_t tier_size;
    switch (tier) {
    case 0:
        tier_size = CSILK_READ_BUF_4KB;
        break;
    case 1:
        tier_size = CSILK_READ_BUF_16KB;
        break;
    default:
        tier_size = CSILK_READ_BUF_64KB;
        break;
    }

    if (wp && wp->read_buf_counts[tier] > 0) {
        buf->base = (char*)wp->read_buf_tiers[tier][--wp->read_buf_counts[tier]];
        buf->len = tier_size;
    } else {
        buf->base = (char*)malloc(tier_size);
        buf->len = tier_size;
    }
}

/** @brief Return a read buffer to the worker-local pool.
 *
 * Pushes the buffer back onto the appropriate tier's free list.
 * If the tier is full, the buffer is freed instead.
 *
 * @param wp     The worker pool (may be NULL — falls back to free).
 * @param base   Buffer pointer to return.
 * @param size   Buffer capacity (used to select the tier).
 */
void
pool_put_read_buf(worker_pool_t* wp, char* base, size_t size)
{
    if (!base || !wp) {
        free(base);
        return;
    }
    int tier = read_buf_tier_index(size);
    if (wp->read_buf_counts[tier] < CSILK_READ_BUF_POOL_SIZE) {
        wp->read_buf_tiers[tier][wp->read_buf_counts[tier]++] = (void*)base;
    } else {
        free(base);
    }
}

/** @brief Pre-allocate buffers for each tier of the read buffer pool.
 *
 * Called once per worker at startup. Each tier gets CSILK_READ_BUF_POOL_SIZE
 * pre-allocated buffers of the corresponding size.
 *
 * @param wp The worker pool to initialise (must not be NULL).
 */
void
_csilk_worker_init_read_buf_pool(worker_pool_t* wp)
{
    const size_t tier_sizes[] = {CSILK_READ_BUF_4KB, CSILK_READ_BUF_16KB, CSILK_READ_BUF_64KB};
    for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
        for (int i = 0; i < CSILK_READ_BUF_POOL_SIZE; i++) {
            void* p = malloc(tier_sizes[tier]);
            if (!p) {
                break;
            }
            wp->read_buf_tiers[tier][wp->read_buf_counts[tier]++] = p;
        }
    }
}
