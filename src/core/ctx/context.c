/**
 * @file context.c
 * @brief Request/response context lifecycle and body I/O.
 *
 * Provides the per-request csilk_ctx_t lifecycle: initialization, cleanup,
 * request/body management, response status and body setup, async/chunked
 * response support, and driver configuration.
 *
 * Key design points:
 *   - The context carries an arena (bump allocator) for all request-scoped
 *     allocations — path strings, query params, handler chains, header
 *     entries. The entire arena is freed in one shot at request end.
 *   - Async mode is signalled via ctx->is_async: when true, the response
 *     is NOT sent in on_message_complete; the handler must call
 *     csilk_send() or csilk_stream() explicitly.
 *   - The context also holds driver pointers (storage, crypto, cipher)
 *     inherited from the server at connection time.
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ctx_internal.h"
#include "../primitives/header_map.h"
#include "../primitives/query.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/** @brief Invoke the next handler in the middleware/route chain.
 *
 * Advances the internal handler index and calls the next non-null handler.
 * If the context has been aborted (via csilk_abort()) or no handlers are
 * registered, this is a no-op.  Each handler is responsible for calling
 * csilk_next() to continue the chain — handlers that set a terminal response
 * (e.g. csilk_string) typically do NOT call csilk_next().
 *
 * @param c The request context. */
void
csilk_next(csilk_ctx_t* c)
{
    if (__builtin_expect(!c || c->aborted || c->panicked, 0)) {
        return;
    }
    csilk_handler_t* handlers = c->handlers;
    if (__builtin_expect(!handlers, 0)) {
        return;
    }
    int next_idx = ++c->handler_index;
    if (__builtin_expect(c->handler_count > 0 && (size_t)next_idx >= c->handler_count, 0)) {
        return;
    }
    csilk_handler_t fn = handlers[next_idx];
    if (__builtin_expect(fn != NULL, 1)) {
        fn(c);
    }
}

/** @brief Abort the handler chain immediately.
 *
 * Sets the aborted flag on the context. Subsequent calls to csilk_next()
 * are ignored. The response is still sent once the current handler returns.
 *
 * @param c The request context.
 * @note This does NOT close the connection — it only prevents further
 *       handlers from executing. */
void
csilk_abort(csilk_ctx_t* c)
{
    c->aborted = 1;
}

/** @brief Dispatch the fully parsed request to the router and middleware chain.
 *
 * Triggers request hooks, executes the router match, invokes the handler
 * chain, and decides whether to send the response synchronously or defer
 * (for async handlers or streaming responses).
 *
 * @param c The request context populated by the HTTP parser. */
CSILK_INTERNAL void
_csilk_dispatch_request(csilk_ctx_t* c)
{
    if (!c || !c->server) {
        return;
    }

    csilk_server_t* server = (csilk_server_t*)c->server;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;

    CSILK_LOG_I("Request: %s %s", c->request.method, c->request.path);
    if (client) {
        csilk_conn_set_state(client, CSILK_CONN_PROCESSING);
    }

    _csilk_trigger_hooks(server, c, CSILK_HOOK_REQUEST_BEGIN);

    /* Acquire router in RCU / EBR read-side critical section */
    csilk_router_t* router = csilk_server_router_acquire(server, &c->router_token);

    if (router && csilk_router_match_ctx(router, c)) {
        CSILK_LOG_D("Route matched, calling next handler");
        csilk_next(c);
    } else {
        CSILK_LOG_W("Route not found: %s", c->request.path);
        if (server->not_found_handler) {
            server->not_found_handler(c);
        } else {
            csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
        }
    }

    if (c->is_async) {
        csilk_client_t* client_ptr = (csilk_client_t*)c->_internal_client;
        if (client_ptr && client_ptr->protocol == CSILK_PROTO_HTTP1) {
            csilk_client_read_stop(client_ptr);
        }
    }

    if (!c->is_async) {
        csilk_server_router_release(server, &c->router_token);
        _csilk_send_response(c);
    }
}

#include <pthread.h>

static _Thread_local csilk_body_pool_t tls_body_pool;
static pthread_key_t                   g_body_pool_key;

static inline int
csilk_body_tier_index(size_t size)
{
    if (size <= CSILK_BODY_POOL_64KB) {
        return 0;
    }
    if (size <= CSILK_BODY_POOL_128KB) {
        return 1;
    }
    if (size <= CSILK_BODY_POOL_256KB) {
        return 2;
    }
    if (size <= CSILK_BODY_POOL_512KB) {
        return 3;
    }
    if (size <= CSILK_BODY_POOL_1MB) {
        return 4;
    }
    return -1;
}

static const size_t k_body_tier_sizes[CSILK_BODY_POOL_TIER_COUNT] = {CSILK_BODY_POOL_64KB,
                                                                     CSILK_BODY_POOL_128KB,
                                                                     CSILK_BODY_POOL_256KB,
                                                                     CSILK_BODY_POOL_512KB,
                                                                     CSILK_BODY_POOL_1MB};

/** @brief Release all cached body buffers in the current thread. */
void
csilk_body_pool_cleanup(void)
{
    for (int tier = 0; tier < CSILK_BODY_POOL_TIER_COUNT; tier++) {
        while (tls_body_pool.tiers[tier].count > 0) {
            void* p = tls_body_pool.tiers[tier].buffers[--tls_body_pool.tiers[tier].count];
            free(p);
        }
    }
}

static void
body_pool_tls_destructor(void* val)
{
    (void)val;
    csilk_body_pool_cleanup();
}

static void
body_pool_init_key(void)
{
    pthread_key_create(&g_body_pool_key, body_pool_tls_destructor);
}

static inline void
body_pool_ensure_cleanup(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, body_pool_init_key);
    if (pthread_getspecific(g_body_pool_key) == NULL) {
        pthread_setspecific(g_body_pool_key, (void*)1);
    }
}

/** @brief Allocate a buffer from the worker/TLS-local HTTP body size-class pool. */
void*
csilk_body_alloc(size_t size, size_t* out_capacity)
{
    int tier = csilk_body_tier_index(size);
    if (tier < 0) {
        void* ptr = malloc(size);
        if (out_capacity) {
            *out_capacity = size;
        }
        return ptr;
    }

    size_t tier_size = k_body_tier_sizes[tier];
    if (out_capacity) {
        *out_capacity = tier_size;
    }

    if (tls_body_pool.tiers[tier].count > 0) {
        return tls_body_pool.tiers[tier].buffers[--tls_body_pool.tiers[tier].count];
    }

    body_pool_ensure_cleanup();

    return malloc(tier_size);
}

/** @brief Return a body buffer to the worker/TLS-local size-class pool or free it. */
void
csilk_body_free(void* ptr, size_t capacity)
{
    if (!ptr) {
        return;
    }

    int tier = csilk_body_tier_index(capacity);
    if (tier >= 0 && capacity == k_body_tier_sizes[tier]) {
        if (tls_body_pool.tiers[tier].count < CSILK_BODY_POOL_MAX_PER_TIER) {
            tls_body_pool.tiers[tier].buffers[tls_body_pool.tiers[tier].count++] = ptr;
            return;
        }
    }

    free(ptr);
}

/** @brief Reallocate / grow a body buffer using the size-class pool. */
void*
csilk_body_realloc(
    void* old_ptr, size_t old_len, size_t old_capacity, size_t new_size, size_t* out_capacity)
{
    if (!old_ptr) {
        return csilk_body_alloc(new_size, out_capacity);
    }

    /* If existing buffer already satisfies the requested new_size, reuse as-is */
    if (old_capacity >= new_size) {
        if (out_capacity) {
            *out_capacity = old_capacity;
        }
        return old_ptr;
    }

    size_t new_cap = 0;
    void*  new_ptr = csilk_body_alloc(new_size, &new_cap);
    if (!new_ptr) {
        return NULL;
    }

    if (old_len > 0) {
        size_t copy_len = old_len < new_cap ? old_len : new_cap;
        memcpy(new_ptr, old_ptr, copy_len);
        if (copy_len < new_cap) {
            ((char*)new_ptr)[copy_len] = '\0';
        }
    } else if (new_cap > 0) {
        ((char*)new_ptr)[0] = '\0';
    }

    csilk_body_free(old_ptr, old_capacity);

    if (out_capacity) {
        *out_capacity = new_cap;
    }
    return new_ptr;
}

/**
 * @brief Release response body memory according to its unified ownership state.
 *
 * Safe and idempotent (can be called repeatedly without double-free).
 * Resets body pointer to NULL, length to 0, capacity to 0, and ownership to CSILK_OWN_NONE.
 */
void
csilk_response_body_release(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    switch (c->response.body_ownership) {
    case CSILK_OWN_POOL:
        if (c->response.body && c->response.body_capacity > 0) {
            csilk_body_free((void*)c->response.body, c->response.body_capacity);
        } else if (c->response.body) {
            free((void*)c->response.body);
        }
        break;
    case CSILK_OWN_HEAP:
    case CSILK_OWN_TRANSFER:
        if (c->response.body) {
            free((void*)c->response.body);
        }
        break;
    case CSILK_OWN_NONE:
    case CSILK_OWN_BORROWED:
    case CSILK_OWN_ARENA:
    default:
        /* No manual deallocation: borrowed view or arena-reclaimed memory */
        break;
    }

    c->response.body = NULL;
    c->response.body_len = 0;
    c->response.body_capacity = 0;
    c->response.body_ownership = CSILK_OWN_NONE;
}

/**
 * @brief Release request body memory according to its unified ownership state.
 *
 * Safe and idempotent. Resets body pointer to NULL, length to 0, capacity to 0, and ownership to CSILK_OWN_NONE.
 */
void
csilk_request_body_release(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    switch (c->request.body_ownership) {
    case CSILK_OWN_POOL:
        if (c->request.body && c->request.body_capacity > 0) {
            csilk_body_free(c->request.body, c->request.body_capacity);
        } else if (c->request.body) {
            free(c->request.body);
        }
        break;
    case CSILK_OWN_HEAP:
    case CSILK_OWN_TRANSFER:
        if (c->request.body) {
            free(c->request.body);
        }
        break;
    case CSILK_OWN_NONE:
    case CSILK_OWN_BORROWED:
    case CSILK_OWN_ARENA:
    default:
        break;
    }

    c->request.body = NULL;
    c->request.body_len = 0;
    c->request.body_capacity = 0;
    c->request.body_ownership = CSILK_OWN_NONE;
}

/** @brief Allocate a response body buffer from the size-class pool and assign it to the context. */
char*
csilk_set_response_body_pooled(csilk_ctx_t* c, size_t size)
{
    if (!c) {
        return NULL;
    }
    size_t cap = 0;
    char*  buf = (char*)csilk_body_alloc(size, &cap);
    if (!buf) {
        return NULL;
    }
    csilk_response_body_release(c);
    c->response.body = buf;
    c->response.body_len = size;
    c->response.body_capacity = cap;
    c->response.body_ownership = CSILK_OWN_POOL;
    return buf;
}

/** @brief Clean up request context resources between requests.
 *
 * Resets the arena allocator for reuse, frees URL path parameters, request
 * body, and path strings. Clears all header maps (request, response, query,
 * form). Cleans up storage items and resets all state flags for the next
 * request. Called after each HTTP request is fully processed.
 *
 * The cleanup only releases resources the current request actually used:
 * heap bodies and the malloc'd path are freed when ownership says so,
 * registered zero-copy receive buffers are returned to the pool, and
 * header maps are zeroed only if they were written this request. Everything
 * else is reclaimed in one shot by csilk_arena_reset().
 *
 * @param c The request context. */
void
csilk_ctx_cleanup(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    /* Release RCU read-side lease if held (e.g. async handler completion, abort, or reset) */
    if (c->router_token.active && c->server) {
        csilk_server_router_release(c->server, &c->router_token);
    }

    /* 1. Deferred callbacks (LIFO) — may release heap memory / fds. */
    csilk_ctx_defer_free(c);

    /* 2. Storage destructors + driver clear — run BEFORE the arena reset:
     *    csilk_set_ex() values can be heap-owned (RAII free_fn) while the
     *    storage nodes themselves are arena-allocated. */
    if (c->storage_head) {
        csilk_storage_item_t* storage_item = c->storage_head;
        while (storage_item) {
            if (storage_item->free_fn && storage_item->value) {
                void (*free_fn)(void*) = storage_item->free_fn;
                free_fn(storage_item->value);
                storage_item->value = NULL;
            }
            storage_item = storage_item->next;
        }
        if (c->storage_driver && c->storage_driver->clear) {
            c->storage_driver->clear(c);
        }
        c->storage_head = NULL;
    }

    /* 3. Zero-copy file response — close the fd if one was opened. */
    if (c->file_fd >= 0) {
        csilk_io_fs_t close_req;
        csilk_io_fs_close(NULL, &close_req, c->file_fd, NULL);
        csilk_io_fs_req_cleanup(&close_req);
        c->file_fd = -1;
    }
    c->file_offset = 0;
    c->file_size = 0;

    /* 4. Request body — released via unified ownership state */
    csilk_request_body_release(c);

    /* 5. Response body — released via unified ownership state */
    csilk_response_body_release(c);
    c->response.status = 0;

    /*
     * request.path is freed here only if it was allocated on the heap (e.g.
     * by test utilities or legacy callers). If it was allocated from the
     * request arena, csilk_arena_reset() will reclaim it.
     */
    if (c->request.path && (!c->arena || !csilk_arena_contains(c->arena, c->request.path))) {
        free(c->request.path);
    }
    c->request.path = NULL;

    /* 6. Zero-copy receive buffers — return to the worker-local pool (or
     *    free) only those actually registered this request. */
    for (int i = 0; i < c->read_buffers_count; i++) {
        char* b = c->read_buffers[i];
        if (!b) {
            continue;
        }
        size_t sz = c->read_buf_sizes[i];
        if (sz > 0 && c->server && c->server->worker_pools) {
            /* Pool-backed buffer — return to worker-local pool instead of free(). */
            extern void    pool_put_read_buf(worker_pool_t * wp, char* base, size_t size);
            worker_pool_t* wp = ((csilk_client_t*)c->_internal_client)->owner_pool;
            pool_put_read_buf(wp, b, sz);
        } else {
            free(b);
        }
        c->read_buffers[i] = NULL;
    }
    if (c->read_buffers && c->read_buffers != c->read_buffers_embedded) {
        free(c->read_buffers);
    }
    if (c->read_buf_sizes && c->read_buf_sizes != c->read_buf_sizes_embedded) {
        free(c->read_buf_sizes);
    }
    c->read_buffers = c->read_buffers_embedded;
    c->read_buffers_count = 0;
    c->read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    c->read_buf_sizes = c->read_buf_sizes_embedded;

    /* 7. Arena reset — reclaims ALL request-scoped arena allocations in one
     *    shot: method, header map nodes/keys/values, query/form params,
     *    storage nodes, defer nodes, param values, arena bodies. */
    if (c->arena) {
        csilk_arena_reset(c->arena);
    } else {
        for (int i = 0; i < c->params_count; i++) {
            free(c->params[i].key);
            free(c->params[i].value);
        }
    }
    c->params_count = 0;

    /* 8. Header maps — clear bucket chains ONLY for maps written this
     *    request (map writers set the `used` flag). Saves three 512-byte
     *    memsets on the typical GET with no query/form/response headers. */
    if (c->request.headers.used) {
        memset(&c->request.headers, 0, sizeof(csilk_header_map_t));
    }
    if (c->request.query_params.used) {
        memset(&c->request.query_params, 0, sizeof(csilk_kv_map_t));
    }
    if (c->request.form_params.used) {
        memset(&c->request.form_params, 0, sizeof(csilk_kv_map_t));
    }
    if (c->response.headers.used) {
        memset(&c->response.headers, 0, sizeof(csilk_header_map_t));
    }

    /* 9. Mutable per-request flags + handler chain state. */
    c->aborted = 0;
    c->panicked = 0;
    c->is_websocket = 0;
    c->is_sse = 0;
    c->is_async = 0;
    c->response_started = 0;
    c->write_paused = 0;
    c->on_drain = NULL;
    c->on_drain_data = NULL;
    c->handler_index = -1;
    c->handlers = NULL;
    c->handler_count = 0;
    c->current_handler = NULL;
    c->on_ws_message = NULL;

    /* 10. Request id — clear only if one was issued this request. */
    if (c->request_id[0]) {
        memset(c->request_id, 0, sizeof(c->request_id));
    }

    /* 11. Request sequence/generation counter — increment to invalidate stale async tokens */
    c->request_seq++;
}

/** @brief Get the request body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-NULL, receives the body length in bytes.
 * @return Pointer to the raw request body, or NULL if no body or NULL context.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_body(csilk_ctx_t* c, size_t* out_len)
{
    if (out_len) {
        *out_len = c ? c->request.body_len : 0;
    }
    return c ? c->request.body : NULL;
}

csilk_view_t
csilk_get_body_view(csilk_ctx_t* c)
{
    if (!c || !c->request.body || c->request.body_len == 0) {
        return csilk_view(NULL, 0);
    }
    return csilk_view(c->request.body, c->request.body_len);
}

const char*
csilk_get_body_str(csilk_ctx_t* c)
{
    if (!c || !c->request.body || c->request.body_len == 0) {
        return "";
    }
    if (c->arena) {
        return csilk_arena_strndup(c->arena, c->request.body, c->request.body_len);
    }
    return c->request.body;
}

/** @brief Get the length of the request body.
 *
 * @param c The request context.
 * @return Body length in bytes, or 0 if the context is NULL or body is empty.
 */
size_t
csilk_get_body_len(csilk_ctx_t* c)
{
    return c ? c->request.body_len : 0;
}

/**
 * @brief Set the HTTP response status code.
 * @param[in] c      Request context (no-op if NULL).
 * @param[in] status HTTP status code to store in the response.
 */
void
csilk_set_status(csilk_ctx_t* c, int status)
{
    if (c) {
        c->response.status = status;
    }
}

/** @brief Get the response body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-NULL, receives the response body length.
 * @return Pointer to the response body, or NULL if no body or NULL context.
 * @note The body may be managed (arena or heap) depending on how it was set.
 *       The caller must not free the returned pointer. */
const char*
csilk_get_response_body(csilk_ctx_t* c, size_t* out_len)
{
    if (!c) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
    if (out_len) {
        *out_len = c->response.body_len;
    }
    return c->response.body;
}

/** @brief Set the response body directly with explicit ownership semantics.
 *
 * Replaces any existing response body. If the old body was heap-managed or transferred,
 * it is freed before replacement.
 *
 * @param c         The request context.
 * @param body      Pointer to the body data (may be NULL).
 * @param len       Body length in bytes.
 * @param ownership Ownership model (CSILK_OWN_BORROWED, CSILK_OWN_ARENA, CSILK_OWN_HEAP, CSILK_OWN_TRANSFER, CSILK_OWN_SHARED).
 */
void
csilk_set_response_body_ex(csilk_ctx_t*      c,
                           const char*       body,
                           size_t            len,
                           csilk_ownership_t ownership)
{
    if (!c) {
        return;
    }
    csilk_response_body_release(c);
    c->response.body = body;
    c->response.body_len = len;
    c->response.body_capacity = 0;
    c->response.body_ownership = body ? ownership : CSILK_OWN_NONE;
}

/** @brief Legacy helper to set response body.
 *
 * @param c       The request context.
 * @param body    Pointer to the body data.
 * @param len     Body length in bytes.
 * @param managed If non-zero, treated as CSILK_OWN_HEAP, else CSILK_OWN_BORROWED.
 */
void
csilk_set_response_body(csilk_ctx_t* c, const char* body, size_t len, int managed)
{
    csilk_set_response_body_ex(c, body, len, managed ? CSILK_OWN_HEAP : CSILK_OWN_BORROWED);
}

/** @brief Query current response body ownership.
 *
 * @param c The request context.
 * @return Ownership model for the current response body.
 */
csilk_ownership_t
csilk_get_response_body_ownership(csilk_ctx_t* c)
{
    return c ? c->response.body_ownership : CSILK_OWN_NONE;
}

/** @brief Configure zero-copy file transmission.
 *
 * @param c      The request context.
 * @param fd     Open file descriptor.
 * @param offset Byte offset to start sending.
 * @param size   Number of bytes to send. */
void
csilk_set_file_response(csilk_ctx_t* c, int fd, size_t offset, size_t size)
{
    if (c) {
        csilk_response_body_release(c);
        c->file_fd = fd;
        c->file_offset = offset;
        c->file_size = size;
    }
}

/** @brief Get the zero-copy file descriptor.
 *
 * @param c The request context.
 * @return File descriptor or -1. */
int
csilk_get_file_fd(csilk_ctx_t* c)
{
    return c ? c->file_fd : -1;
}

/** @brief Initialize a request context.
 *
 * Sets up default values for all fields. Should be called for both
 * static (embedded in client) and dynamic (H2 stream) contexts.
 *
 * @param c       The context to initialize.
 * @param s       The owning server instance.
 * @param client  The underlying connection object (csilk_client_t*). */
CSILK_INTERNAL void
_csilk_ctx_init(csilk_ctx_t* c, struct csilk_server_s* s, void* client)
{
    if (!c) {
        return;
    }
    uint64_t old_seq = c->request_seq;
    memset(c, 0, sizeof(csilk_ctx_t));
    c->request_seq = old_seq ? old_seq + 1 : 1;
    c->handler_index = -1;
    c->file_fd = -1;
    c->_internal_client = client;
    c->server = s;
    c->read_buffers = c->read_buffers_embedded;
    c->read_buffers_count = 0;
    c->read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    c->read_buf_sizes = c->read_buf_sizes_embedded;
    c->write_high_water_mark = CSILK_WRITE_HWM_DEFAULT;
    c->write_low_water_mark = CSILK_WRITE_LWM_DEFAULT;
    c->max_write_buffer_size = CSILK_WRITE_MAX_BUFFER_DEFAULT;
    c->write_paused = 0;
    c->on_drain = NULL;
    c->on_drain_data = NULL;
    if (s) {

        c->storage_driver = s->storage_driver;
        c->crypto_driver = s->crypto_driver;
        c->cipher_driver = s->cipher_driver;
    }
}

/** @brief Specialized stream-scoped context initialiser for HTTP/2 multiplexing. */
CSILK_INTERNAL void
_csilk_stream_ctx_init(csilk_ctx_t* c, csilk_client_t* client, int32_t stream_id)
{
    if (!c || !client) {
        return;
    }
    uint64_t       old_seq = c->request_seq;
    uint64_t       old_stream_gen = c->stream_gen;
    csilk_arena_t* saved_arena = c->arena;
    memset(c, 0, sizeof(csilk_ctx_t));
    c->arena = saved_arena;
    c->request_seq = old_seq ? old_seq + 1 : 1;
    c->stream_gen = old_stream_gen ? old_stream_gen + 1 : 1;
    c->stream_state = CSILK_STREAM_STATE_ACTIVE;
    c->handler_index = -1;
    c->file_fd = -1;
    c->_internal_client = client;
    c->server = client->server;
    c->stream_id = stream_id;
    c->h2_stream_owner = client;
    atomic_init(&c->stream_ref, 1);
    c->stream_closed = 0;
    atomic_init(&c->stream_destroy_pending, 0);
    c->read_buffers = c->read_buffers_embedded;
    c->read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
    c->read_buf_sizes = c->read_buf_sizes_embedded;
    c->write_high_water_mark = CSILK_WRITE_HWM_DEFAULT;
    c->write_low_water_mark = CSILK_WRITE_LWM_DEFAULT;
    c->max_write_buffer_size = CSILK_WRITE_MAX_BUFFER_DEFAULT;
    if (client->server) {
        c->storage_driver = client->server->storage_driver;
        c->crypto_driver = client->server->crypto_driver;
        c->cipher_driver = client->server->cipher_driver;
    }
}

/** @brief Register a zero-copy read buffer for lifetime management across the request.
 *
 * Dynamically expands buffer array if number of TCP reads exceeds embedded capacity.
 *
 * @param c    The request context.
 * @param base Pointer to malloc'd buffer.
 * @return 0 on success, -1 on memory allocation failure. */
CSILK_INTERNAL int
_csilk_ctx_register_read_buffer(csilk_ctx_t* c, char* base)
{
    return _csilk_ctx_register_pooled_read_buffer(c, base, 0);
}

/** @brief Register a pool-backed read buffer, tracking its size for pool return on cleanup.
 *
 * Records the buffer size in a parallel array so that csilk_ctx_cleanup() can
 * route it back to the worker-local read buffer pool instead of calling free().
 * Size == 0 means the buffer is malloc-owned and must be freed normally.
 *
 * @param c      The request context.
 * @param base   Buffer pointer (must remain valid until cleanup).
 * @param size   Buffer capacity (pool tier size), or 0 for non-pooled buffers.
 * @return 0 on success, -1 on allocation failure. */
CSILK_INTERNAL int
_csilk_ctx_register_pooled_read_buffer(csilk_ctx_t* c, char* base, size_t size)
{
    if (!c || !base) {
        return -1;
    }
    if (c->read_buffers_capacity <= 0 || !c->read_buffers) {
        c->read_buffers = c->read_buffers_embedded;
        c->read_buffers_capacity = CSILK_READ_BUF_EMBEDDED;
        c->read_buffers_count = 0;
        c->read_buf_sizes = c->read_buf_sizes_embedded;
    }
    if (c->read_buffers_count >= c->read_buffers_capacity) {
        int     new_cap = c->read_buffers_capacity * 2;
        char**  new_arr = NULL;
        size_t* new_sizes = NULL;
        if (c->read_buffers == c->read_buffers_embedded) {
            new_arr = malloc((size_t)new_cap * sizeof(char*));
            new_sizes = malloc((size_t)new_cap * sizeof(size_t));
            if (!new_arr || !new_sizes) {
                free(new_arr);
                free(new_sizes);
                return -1;
            }
            memcpy(
                new_arr, c->read_buffers_embedded, (size_t)c->read_buffers_count * sizeof(char*));
        } else {
            new_arr = realloc(c->read_buffers, (size_t)new_cap * sizeof(char*));
            if (!new_arr) {
                return -1;
            }
            new_sizes = realloc(c->read_buf_sizes, (size_t)new_cap * sizeof(size_t));
            if (!new_sizes) {
                free(new_arr);
                return -1;
            }
        }
        c->read_buffers = new_arr;
        c->read_buf_sizes = new_sizes;
        c->read_buffers_capacity = new_cap;
    }
    c->read_buffers[c->read_buffers_count] = base;
    c->read_buf_sizes[c->read_buffers_count++] = size;
    return 0;
}

/** @brief Set the storage driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_storage_driver(csilk_ctx_t* c, csilk_storage_driver_t* driver)
{
    if (c) {
        c->storage_driver = driver;
    }
}

/** @brief Set the crypto driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_crypto_driver(csilk_ctx_t* c, csilk_crypto_driver_t* driver)
{
    if (c) {
        c->crypto_driver = driver;
    }
}

/** @brief Set the cipher driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_cipher_driver(csilk_ctx_t* c, csilk_cipher_driver_t* driver)
{
    if (c) {
        c->cipher_driver = driver;
    }
}
