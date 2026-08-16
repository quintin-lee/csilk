# Connection Module Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split monolithic `src/core/server/connection.c` (1071 lines) into 6 focused files with clear responsibilities.

**Architecture:** Extract connection pool, state machine, timers, close logic, and I/O callbacks into separate files. The main `connection.c` becomes a thin wrapper exporting public APIs.

**Tech Stack:** C23, CMake, libuv/io_uring backend abstraction

---

## File Structure

### New Files to Create
- `src/core/server/connection_pool.c` — client/arena/buffer pooling
- `src/core/server/connection_state.c` — connection state machine
- `src/core/server/connection_timer.c` — timer callbacks
- `src/core/server/connection_close.c` — close/destroy logic
- `src/core/server/connection_io.c` — on_new_connection, on_read, I/O

### Files to Modify
- `src/core/server/connection.c` — reduced to thin wrapper (~50 lines)
- `src/core/internal/srv_impl.h` — update declarations
- `CMakeLists.txt` — add new source files

---

## Task 1: Create connection_state.c

**Files:**
- Create: `src/core/server/connection_state.c`
- Modify: `src/core/server/connection.c` (remove state functions)

- [ ] **Step 1: Create connection_state.c with state functions**

```c
/**
 * @file connection_state.c
 * @brief Connection lifecycle state machine.
 */

#include "../internal/srv_internal.h"

const char*
csilk_conn_state_str(csilk_conn_state_t state)
{
    switch (state) {
    case CSILK_CONN_INIT:
        return "INIT";
    case CSILK_CONN_ACCEPTED:
        return "ACCEPTED";
    case CSILK_CONN_TLS:
        return "TLS";
    case CSILK_CONN_READING:
        return "READING";
    case CSILK_CONN_PROCESSING:
        return "PROCESSING";
    case CSILK_CONN_WRITING:
        return "WRITING";
    case CSILK_CONN_STREAMING:
        return "STREAMING";
    case CSILK_CONN_CLOSING:
        return "CLOSING";
    case CSILK_CONN_CLOSED:
        return "CLOSED";
    default:
        return "UNKNOWN";
    }
}

void
csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t old_state = client->state;
    if (old_state == new_state) {
        return;
    }

    /* Invariant: once CLOSED, connection cannot transition except to INIT (pool reuse) */
    if (old_state == CSILK_CONN_CLOSED && new_state != CSILK_CONN_INIT) {
        CSILK_LOG_W("Conn %p: illegal state transition from CLOSED to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    /* Invariant: once CLOSING, only allowed next state is CLOSED */
    if (old_state == CSILK_CONN_CLOSING && new_state != CSILK_CONN_CLOSED) {
        CSILK_LOG_D("Conn %p: ignored transition from CLOSING to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    CSILK_LOG_T("Conn %p state: %s -> %s",
                (void*)client,
                csilk_conn_state_str(old_state),
                csilk_conn_state_str(new_state));
    client->state = new_state;
}

csilk_conn_state_t
csilk_conn_get_state(const csilk_client_t* client)
{
    return client ? client->state : CSILK_CONN_CLOSED;
}
```

- [ ] **Step 2: Remove state functions from connection.c**

In `src/core/server/connection.c`, remove lines 57-132 (the three state functions).

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/server/connection_state.c src/core/server/connection.c
git commit -m "refactor(server): 🔄 extract connection state machine to connection_state.c"
```

---

## Task 2: Create connection_pool.c

**Files:**
- Create: `src/core/server/connection_pool.c`
- Modify: `src/core/server/connection.c` (remove pool functions)

- [ ] **Step 1: Create connection_pool.c with pool functions**

```c
/**
 * @file connection_pool.c
 * @brief Connection pool, arena pool, and read buffer pool management.
 */

#include "../internal/srv_internal.h"

/* --- Buffer allocation --- */

/** @brief Forward declaration for pool_get_read_buf. */
static void pool_get_read_buf(worker_pool_t* wp, size_t suggested_size, csilk_io_buf_t* buf);

/** @brief I/O buffer allocation callback. */
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

static csilk_client_t*
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
        client->state = CSILK_CONN_INIT;
        client->generation = gen;
#ifdef CSILK_USE_URING
        client->handle.generation = gen;
        client->timer.generation = gen;
        client->read_timer.generation = gen;
        client->write_timer.generation = gen;
        client->request_timer.generation = gen;
        client->async_ref = 0;
        client->close_pending = 0;
        client->ctx.conn_closed = 0;
#endif
        client->ctx.file_fd = -1;
    }
    return client;
}

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
    client->state = CSILK_CONN_INIT;
    client->close_pending = 0;
    client->async_ref = 0;
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
    memset(client->ctx.request_id, 0, sizeof(client->ctx.request_id));
}

static void
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

static csilk_arena_t*
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

static void
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
```

- [ ] **Step 2: Remove pool functions from connection.c**

In `src/core/server/connection.c`, remove lines 26-460 (buffer allocation through read buffer pool initialization).

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/server/connection_pool.c src/core/server/connection.c
git commit -m "refactor(server): 🔄 extract connection pool to connection_pool.c"
```

---

## Task 3: Create connection_timer.c

**Files:**
- Create: `src/core/server/connection_timer.c`
- Modify: `src/core/server/connection.c` (remove timer callbacks)

- [ ] **Step 1: Create connection_timer.c**

```c
/**
 * @file connection_timer.c
 * @brief Timer callbacks for connection lifecycle.
 */

#include "../internal/srv_internal.h"

/** @brief Close callback for timer handles. */
static void
on_timer_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!client) {
        return;
    }
    client->close_pending--;
    if (client->close_pending > 0) {
        return;
    }
    if (client->async_ref > 0) {
        return;
    }
    /* Forward declaration - defined in connection_close.c */
    extern void client_destroy(csilk_client_t* client);
    client_destroy(client);
}

/** @brief Timer callback: idle timeout. */
void
on_idle_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to idle timeout");
        extern void on_close(csilk_io_handle_t* handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: read timeout. */
void
on_read_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to read timeout");
        extern void on_close(csilk_io_handle_t* handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: write timeout. */
void
on_write_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        extern void on_close(csilk_io_handle_t* handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}
```

- [ ] **Step 2: Remove timer callbacks from connection.c**

In `src/core/server/connection.c`, remove lines 616-738 (on_timer_close, on_idle_timeout, on_read_timeout, on_write_timeout).

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/server/connection_timer.c src/core/server/connection.c
git commit -m "refactor(server): 🔄 extract timer callbacks to connection_timer.c"
```

---

## Task 4: Create connection_close.c

**Files:**
- Create: `src/core/server/connection_close.c`
- Modify: `src/core/server/connection.c` (remove close functions)

- [ ] **Step 1: Create connection_close.c**

```c
/**
 * @file connection_close.c
 * @brief Connection close and teardown logic.
 */

#include "../internal/srv_internal.h"

/* --- Active client list --- */

static void
client_list_add(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
    worker_pool_t* wp = client->owner_pool;
    client->next = wp->active_clients;
    client->prev = NULL;
    if (wp->active_clients) {
        wp->active_clients->prev = client;
    }
    wp->active_clients = client;
}

static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
    (void)server;
    worker_pool_t* wp = client->owner_pool;
    if (!wp) {
        return;
    }
    if (client->prev) {
        client->prev->next = client->next;
    } else if (wp->active_clients == client) {
        wp->active_clients = client->next;
    }
    if (client->next) {
        client->next->prev = client->prev;
    }
    client->next = client->prev = NULL;
}

/* --- Timer close --- */

/** @brief Final teardown step for a client connection. */
static void
client_destroy(csilk_client_t* client)
{
    if (!client || client->state == CSILK_CONN_CLOSED) {
        return;
    }
#ifdef CSILK_USE_URING
    if (client->handle.fd >= 0) {
        close(client->handle.fd);
        client->handle.fd = -1;
    }
#endif
    if (client->server) {
        atomic_fetch_sub(&client->server->active_connections, 1);
    }
    csilk_conn_set_state(client, CSILK_CONN_CLOSED);
    csilk_ctx_cleanup(&client->ctx);
    if (client->ctx.arena) {
        extern void pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena);
        pool_put_arena(client->owner_pool, client->ctx.arena);
    }
    extern void pool_put(worker_pool_t* wp, csilk_client_t* client);
    pool_put(client->owner_pool, client);
}

/** @brief Get the I/O event loop associated with the request context. */
CSILK_INTERNAL csilk_io_loop_t*
_csilk_ctx_loop(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return csilk_io_default_loop();
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    return client->handle.loop;
}

/** @brief Increment the async reference counter. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_incr(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    client->async_ref++;
}

/** @brief Decrement the async reference counter; destroy client if last ref. */
CSILK_INTERNAL void
_csilk_ctx_async_ref_decr(csilk_ctx_t* c)
{
    if (!c || !c->server || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    client->async_ref--;
    if (client->async_ref <= 0) {
#ifdef CSILK_USE_URING
        if ((client->handle.flags & CSILK_IO_HANDLE_CLOSING) && client->handle.fd >= 0) {
            close(client->handle.fd);
            client->handle.fd = -1;
        }
#endif
        if (client->close_pending <= 0 && c->conn_closed) {
            client_destroy(client);
        }
    }
}

/* --- Connection close --- */

CSILK_INTERNAL void
on_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client) {
        CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);
        _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
        client_list_remove(client->server, client);
        client->ctx.conn_closed = 1;
        csilk_io_timer_stop(&client->timer);

        csilk_io_timer_stop(&client->read_timer);
        csilk_io_timer_stop(&client->write_timer);
        csilk_io_timer_stop(&client->request_timer);

        client->close_pending = 4;
        csilk_io_handle_t* timers[] = {(csilk_io_handle_t*)&client->timer,
                                       (csilk_io_handle_t*)&client->read_timer,
                                       (csilk_io_handle_t*)&client->write_timer,
                                       (csilk_io_handle_t*)&client->request_timer};
        int                closed_count = 0;
        for (int i = 0; i < 4; i++) {
            if (csilk_io_is_closing(timers[i])) {
                client->close_pending--;
            } else {
                closed_count++;
                timers[i]->data = client;
                csilk_io_close(timers[i], on_timer_close);
            }
        }
        if (closed_count == 0 && client->close_pending <= 0) {
            if (client->async_ref > 0) {
                return;
            }
            client_destroy(client);
        }
    }
}
```

- [ ] **Step 2: Remove close functions from connection.c**

In `src/core/server/connection.c`, remove lines 516-688 (client_list_add/remove, on_timer_close, client_destroy, ctx helpers, on_close).

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/server/connection_close.c src/core/server/connection.c
git commit -m "refactor(server): 🔄 extract close logic to connection_close.c"
```

---

## Task 5: Create connection_io.c

**Files:**
- Create: `src/core/server/connection_io.c`
- Modify: `src/core/server/connection.c` (remove I/O functions)

- [ ] **Step 1: Create connection_io.c**

```c
/**
 * @file connection_io.c
 * @brief Connection I/O callbacks: accept, read, reject.
 */

#include "../internal/srv_internal.h"

/* --- Rejected connection --- */

#ifndef CSILK_USE_URING
static void
on_rejected_close(csilk_io_handle_t* handle)
{
    free(handle);
}
#endif

static void
reject_connection(csilk_io_stream_t* server_stream)
{
#ifdef CSILK_USE_URING
    csilk_io_tcp_t tmp;
    csilk_io_tcp_init(server_stream->loop, &tmp);
    if (csilk_io_accept(server_stream, (csilk_io_stream_t*)&tmp) == 0) {
        csilk_io_close((csilk_io_handle_t*)&tmp, NULL);
    }
#else
    csilk_io_tcp_t* tmp = malloc(sizeof(csilk_io_tcp_t));
    if (tmp) {
        csilk_io_tcp_init(server_stream->loop, tmp);
        if (csilk_io_accept(server_stream, (csilk_io_stream_t*)tmp) == 0) {
            extern void on_rejected_close(csilk_io_handle_t* handle);
            csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
        } else {
            extern void on_rejected_close(csilk_io_handle_t* handle);
            csilk_io_close((csilk_io_handle_t*)tmp, on_rejected_close);
        }
    }
#endif
}

/* --- Accept new connection --- */

void
on_new_connection(csilk_io_stream_t* server_stream, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Connection: new connection error: %s", csilk_io_strerror(status));
        return;
    }

    worker_pool_t*  wp = (worker_pool_t*)server_stream->data;
    csilk_server_t* server = wp->server;

    if (_csilk_server_try_acquire_connection(server) < 0) {
        reject_connection(server_stream);
        return;
    }

    extern csilk_client_t* pool_get(worker_pool_t* wp);
    csilk_client_t* client = pool_get(wp);
    if (!client) {
        _csilk_server_release_connection(server);
        reject_connection(server_stream);
        return;
    }

    client->server = server;
    client->owner_pool = wp;
    int r = csilk_io_tcp_init(server_stream->loop, &client->handle);
    if (r < 0) {
        CSILK_LOG_E("Connection: csilk_io_tcp_init error: %s", csilk_io_strerror(r));
        _csilk_server_release_connection(server);
        extern void pool_put(worker_pool_t* wp, csilk_client_t* client);
        pool_put(wp, client);
        return;
    }
    client->handle.data = client;

    _csilk_ctx_init(&client->ctx, server, client);
    extern csilk_arena_t* pool_get_arena(worker_pool_t* wp);
    client->ctx.arena = pool_get_arena(wp);

    extern void client_list_add(csilk_server_t* server, csilk_client_t* client);
    client_list_add(server, client);

    if (csilk_io_accept(server_stream, (csilk_io_stream_t*)&client->handle) == 0) {
        CSILK_LOG_D("Connection: accepted new TCP connection (client pointer: %p)", (void*)client);
        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
        if (server->config.tcp_nodelay) {
            csilk_io_tcp_nodelay(&client->handle, 1);
        }
        client->protocol = CSILK_PROTO_HTTP1;
        llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
        client->parser.data = client;

        _csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

        if (server->ssl_ctx) {
            CSILK_LOG_D("Connection: setting up TLS for connection: %p", (void*)client);
            csilk_conn_set_state(client, CSILK_CONN_TLS);
            extern int setup_client_tls(csilk_client_t* client);
            if (setup_client_tls(client) < 0) {
                extern void on_close(csilk_io_handle_t* handle);
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
                return;
            }
        }

        /* Timer initialization */
        csilk_io_timer_init(server_stream->loop, &client->timer);
        client->timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->read_timer);
        client->read_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->write_timer);
        client->write_timer.data = client;
        csilk_io_timer_init(server_stream->loop, &client->request_timer);
        client->request_timer.data = client;

        CSILK_LOG_T("Connection: connection timers initialized, starting read listener");
        if (server->config.read_timeout_ms > 0) {
            csilk_io_timer_start(
                &client->read_timer, on_read_timeout, server->config.read_timeout_ms, 0);
        }
        if (server->config.request_timeout_ms > 0) {
            csilk_io_timer_start(
                &client->request_timer, on_read_timeout, server->config.request_timeout_ms, 0);
        }

        if (!server->ssl_ctx) {
            csilk_conn_set_state(client, CSILK_CONN_READING);
        }

        r = csilk_io_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
        if (r < 0) {
            CSILK_LOG_E("Connection: csilk_io_read_start error: %s", csilk_io_strerror(r));
            if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
                extern void on_close(csilk_io_handle_t* handle);
                csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
            }
        }
    } else {
        client_list_remove(server, client);
        _csilk_server_release_connection(server);
        if (client->ctx.arena) {
            extern void pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena);
            pool_put_arena(wp, client->ctx.arena);
            client->ctx.arena = NULL;
        }
        extern void pool_put(worker_pool_t* wp, csilk_client_t* client);
        pool_put(wp, client);
    }
}

/* --- TCP read --- */

void
on_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf)
{
    csilk_client_t* client = (csilk_client_t*)stream->data;
    char*           base = buf->base;
    int             is_registered = 0;

    if (!client || client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        if (base) {
            extern void pool_put_read_buf(worker_pool_t* wp, char* base, size_t size);
            pool_put_read_buf(NULL, base, buf->len);
        }
        return;
    }

    csilk_io_timer_stop(&client->timer);
    if (client->server->config.read_timeout_ms > 0) {
        csilk_io_timer_start(
            &client->read_timer, on_read_timeout, client->server->config.read_timeout_ms, 0);
    }
    if (nread > 0) {
        if (client->ssl) {
            BIO_write(client->read_bio, base, (int)nread);
            extern void process_tls_read(csilk_client_t* client);
            process_tls_read(client);
        } else if (client->ctx.is_websocket) {
            csilk_conn_set_state(client, CSILK_CONN_STREAMING);
            csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
        } else {
            csilk_conn_set_state(client, CSILK_CONN_READING);

            if (_csilk_ctx_register_read_buffer(&client->ctx, base) == 0) {
                is_registered = 1;
            } else {
                CSILK_LOG_E("Connection: failed to register read buffer, out of memory");
            }

            enum llhttp_errno err = llhttp_execute(&client->parser, base, nread);
            if (err == HPE_CLOSED_CONNECTION) {
                llhttp_init(&client->parser, HTTP_REQUEST, &client->server->settings);
                client->parser.data = client;
            } else if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
                CSILK_LOG_E("Connection: HTTP parse error: %s %s",
                            llhttp_errno_name(err),
                            client->parser.reason ? client->parser.reason : "unknown reason");

                if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
                    extern void on_close(csilk_io_handle_t* handle);
                    csilk_io_close((csilk_io_handle_t*)stream, on_close);
                }
            }
        }

    } else if (nread < 0) {
        if (nread != -1 && nread != -4095 /* UV_EOF */) {
            CSILK_LOG_E("Connection: read error: %s", csilk_io_err_name((int)nread));
        }
        if (!csilk_io_is_closing((csilk_io_handle_t*)stream)) {
            extern void on_close(csilk_io_handle_t* handle);
            csilk_io_close((csilk_io_handle_t*)stream, on_close);
        }
    }

    if (base && !is_registered) {
        extern void pool_put_read_buf(worker_pool_t* wp, char* base, size_t size);
        pool_put_read_buf(client->owner_pool, base, buf->len);
    }
}

/* --- Get client IP --- */

const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
    if (!c || !c->_internal_client) {
        return NULL;
    }
    csilk_client_t*         client = (csilk_client_t*)c->_internal_client;
    struct sockaddr_storage addr;
    int                     len = sizeof(addr);
    if (csilk_io_tcp_getpeername(&client->handle, (struct sockaddr*)&addr, &len) == 0) {
        char ip[46];
        if (addr.ss_family == AF_INET) {
            csilk_io_ip4_name((const struct sockaddr_in*)&addr, ip, sizeof(ip));
        } else {
            csilk_io_ip6_name((const struct sockaddr_in6*)&addr, ip, sizeof(ip));
        }
        return csilk_arena_strdup(c->arena, ip);
    }

    return NULL;
}

void
csilk_client_read_start(csilk_client_t* client)
{
    csilk_io_read_start((csilk_io_stream_t*)&client->handle, alloc_buffer, on_read);
}

void
csilk_client_read_stop(csilk_client_t* client)
{
    csilk_io_read_stop((csilk_io_stream_t*)&client->handle);
}
```

- [ ] **Step 2: Remove I/O functions from connection.c**

In `src/core/server/connection.c`, remove lines 740-1071 (rejected connection through end).

- [ ] **Step 3: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/server/connection_io.c src/core/server/connection.c
git commit -m "refactor(server): 🔄 extract I/O callbacks to connection_io.c"
```

---

## Task 6: Update connection.c to thin wrapper

**Files:**
- Modify: `src/core/server/connection.c`

- [ ] **Step 1: Replace connection.c with thin wrapper**

```c
/**
 * @file connection.c
 * @brief Connection module public API.
 *
 * This file re-exports public symbols from the split connection modules.
 * See:
 *   connection_pool.c  - pool management
 *   connection_state.c - state machine
 *   connection_timer.c - timer callbacks
 *   connection_close.c - close/destroy logic
 *   connection_io.c    - I/O callbacks
 */

#include "csilk/core/internal.h"

/* Re-export public API */
const char* csilk_conn_state_str(csilk_conn_state_t state);
csilk_conn_state_t csilk_conn_get_state(const csilk_client_t* client);
void csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state);

const char* csilk_get_client_ip(csilk_ctx_t* c);
void csilk_client_read_start(csilk_client_t* client);
void csilk_client_read_stop(csilk_client_t* client);
```

- [ ] **Step 2: Build and run tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/core/server/connection.c
git commit -m "refactor(server): 🔄 reduce connection.c to thin wrapper"
```

---

## Task 7: Update CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt` (or relevant cmake module)

- [ ] **Step 1: Find where server sources are listed**

Search for `connection.c` in CMakeLists.txt or cmake/*.cmake files.

- [ ] **Step 2: Add new source files**

Replace `src/core/server/connection.c` with the new files in the source list:
```cmake
src/core/server/connection_pool.c
src/core/server/connection_state.c
src/core/server/connection_timer.c
src/core/server/connection_close.c
src/core/server/connection_io.c
src/core/server/connection.c
```

- [ ] **Step 3: Build and run all tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --timeout 10 --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: 📦 add connection module split files to build"
```

---

## Task 8: Final Verification

- [ ] **Step 1: Run full test suite**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --timeout 10 --output-on-failure
```

- [ ] **Step 2: Run format check**

```bash
cmake --build build --target check-format
```

- [ ] **Step 3: Run clang-tidy**

```bash
cmake --build build --target tidy
```

- [ ] **Step 4: Verify file sizes**

```bash
wc -l src/core/server/connection*.c
```

Expected output:
```
  ~50 src/core/server/connection.c
 ~250 src/core/server/connection_pool.c
  ~80 src/core/server/connection_state.c
 ~200 src/core/server/connection_timer.c
 ~180 src/core/server/connection_close.c
 ~300 src/core/server/connection_io.c
```

- [ ] **Step 5: Final commit if needed**

```bash
git add -A
git commit -m "refactor(server): 🔄 finalize connection module split"
```
