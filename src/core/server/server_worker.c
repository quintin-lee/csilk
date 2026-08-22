/**
 * @file server_worker.c
 * @brief Multi-worker threading — SO_REUSEPORT binding, CPU pinning, task dispatch.
 *
 * @copyright MIT License
 */

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32
#include <sched.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

#ifndef UV_HANDLE_BOUND
#define UV_HANDLE_BOUND 0x00002000
#endif

/* --- Producer-Safe Dispatch Task Cache --- */

#define CSILK_DISPATCH_TLS_CAPACITY 64

typedef struct {
    csilk_dispatch_task_t* items[CSILK_DISPATCH_TLS_CAPACITY];
    int                    count;
} csilk_dispatch_tls_cache_t;

static _Thread_local csilk_dispatch_tls_cache_t g_dispatch_tls_cache = {.count = 0};

static pthread_key_t  g_dispatch_tls_key;
static pthread_once_t g_dispatch_tls_once = PTHREAD_ONCE_INIT;

static void
dispatch_tls_cleanup(void* unused)
{
    (void)unused;
    while (g_dispatch_tls_cache.count > 0) {
        csilk_dispatch_task_t* task = g_dispatch_tls_cache.items[--g_dispatch_tls_cache.count];
        free(task);
    }
}

static void
dispatch_init_tls_key(void)
{
    pthread_key_create(&g_dispatch_tls_key, dispatch_tls_cleanup);
    atexit(_csilk_dispatch_pool_cleanup);
}

static inline void
ensure_dispatch_tls_registered(void)
{
    pthread_once(&g_dispatch_tls_once, dispatch_init_tls_key);
    if (!pthread_getspecific(g_dispatch_tls_key)) {
        pthread_setspecific(g_dispatch_tls_key, (void*)1);
    }
}

csilk_dispatch_task_t*
_csilk_dispatch_task_alloc(void)
{
    ensure_dispatch_tls_registered();

    /* Fast path from thread-local cache */
    if (g_dispatch_tls_cache.count > 0) {
        return g_dispatch_tls_cache.items[--g_dispatch_tls_cache.count];
    }

    /* Direct allocator fallback */
    csilk_dispatch_task_t* task = (csilk_dispatch_task_t*)malloc(sizeof(csilk_dispatch_task_t));
    if (task) {
        atomic_init(&task->pool_next, NULL);
    }
    return task;
}

void
_csilk_dispatch_task_free(csilk_dispatch_task_t* task)
{
    if (!task) {
        return;
    }

    ensure_dispatch_tls_registered();

    /* Fast path to thread-local cache */
    if (g_dispatch_tls_cache.count < CSILK_DISPATCH_TLS_CAPACITY) {
        g_dispatch_tls_cache.items[g_dispatch_tls_cache.count++] = task;
        return;
    }

    /* Free to OS */
    free(task);
}

void
_csilk_dispatch_pool_cleanup(void)
{
    /* Clean up caller thread's TLS cache */
    dispatch_tls_cleanup(NULL);
}

void
_csilk_worker_drain_dispatch(worker_pool_t* wp)
{
    if (!wp) {
        return;
    }

    csilk_lfq_node_t* node = csilk_lfq_dequeue(&wp->dispatch_queue);
    while (node) {
        csilk_dispatch_task_t* task = (csilk_dispatch_task_t*)node;
        csilk_client_t*        client = task->client;
        if (task->cb) {
            task->cb(task->arg);
        }
        if (client) {
            csilk_client_unref(client);
        }
        _csilk_dispatch_task_free(task);
        node = csilk_lfq_dequeue(&wp->dispatch_queue);
    }
}

/**
 * @brief Drain and invoke tasks queued on a worker's dispatch async handle.
 * @param[in] handle async handle whose data points at the worker_pool_t.
 * @note Dequeues every csilk_dispatch_task_t from the worker dispatch queue and
 *       runs its callback, freeing each task afterwards. No-op if the pool is
 *       NULL. Runs on the worker loop when the async is signaled.
 */
static void
on_dispatch_async(csilk_io_async_t* handle)
{
    worker_pool_t* wp = (worker_pool_t*)handle->data;
    _csilk_worker_drain_dispatch(wp);
}

/**
 * @brief Initialize the worker-pool dispatch async channel.
 * @param[in] wp   Worker pool whose dispatch queue is initialized.
 * @param[in] loop Event loop on which the dispatch async is registered.
 * @note Initializes the lock-free dispatch queue and registers on_dispatch_async
 *       as the async callback; the async handle's data is set to the pool.
 */
void
_csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop)
{
    csilk_lfq_init(&wp->dispatch_queue);
    csilk_io_async_init(loop, &wp->dispatch_async, on_dispatch_async);
    wp->dispatch_async.data = wp;
}

/**
 * @brief Queue a callback to run on the owning worker's event loop.
 *
 * @param[in] c   Request context whose internal client identifies the target worker.
 * @param[in] cb  Callback function to invoke with @p arg on the target worker loop.
 * @param[in] arg User closure argument passed to @p cb.
 *
 * @note Thread-Safety: Safe to call from any thread or worker. This is the official mechanism
 *       for cross-worker communication to preserve strict single-thread confinement of
 *       wp->active_clients and connection state.
 * @note Allocates a task from the producer-safe task pool (or falls back to malloc),
 *       enqueues it on the worker's lock-free dispatch queue, and signals the async handle.
 */
void
csilk_dispatch(csilk_ctx_t* c, void (*cb)(void* arg), void* arg)
{
    if (!c || !c->_internal_client || !cb) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client->owner_pool) {
        return;
    }
    worker_pool_t* wp = client->owner_pool;

    csilk_dispatch_task_t* task = _csilk_dispatch_task_alloc();
    if (!task) {
        return;
    }
    task->cb = cb;
    task->arg = arg;
    task->client = client;
    csilk_client_ref(client);
    csilk_lfq_enqueue(&wp->dispatch_queue, &task->lfq_node);

    csilk_io_async_send(&wp->dispatch_async);
}

/* --- CPU pinning --- */

/**
 * @brief Pin the calling thread to a CPU core via thread affinity.
 * @param[in] core_id Desired core index (wrapped modulo online core count).
 * @note No-op on Windows or when the wrapped core index is out of range.
 *       Logs the resulting pin via CSILK_LOG_I.
 */
static void
pin_thread_to_core(int core_id)
{
#ifndef _WIN32
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores <= 0) {
        return;
    }
    int  target_core = (int)(core_id % num_cores);
    char cpuset[128] = {0};
    if (target_core < 128) {
        cpuset[target_core] = 1;
        csilk_thread_t tid = csilk_thread_self();
        csilk_thread_setaffinity(&tid, cpuset, NULL, 128);
        CSILK_LOG_I("Server: Pinned worker thread %d to CPU core %d", core_id, target_core);
    }
#else
    (void)core_id;
#endif
}

/* --- Bind and listen --- */

/** @brief Create, bind, and listen on a TCP socket with optional SO_REUSEPORT. */
int
bind_and_listen(csilk_io_loop_t* loop,
                csilk_io_tcp_t*  out_handle,
                int              port,
                int              backlog,
                bool             reuseport,
                int              worker_index)
{
#ifndef _WIN32
    if (reuseport) {
        int fd;
#ifdef __APPLE__
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
#else
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
#endif
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

#if defined(__linux__) && defined(SO_INCOMING_CPU)
        if (worker_index >= 0) {
            long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
            if (num_cores > 0) {
                int cpu = (int)(worker_index % num_cores);
                setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
            }
        }
#endif
        struct sockaddr_in addr;
        csilk_io_ip4_addr("0.0.0.0", port, &addr);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
        if (listen(fd, backlog) < 0) {
            close(fd);
            return -1;
        }
        void* data = out_handle->data;
        int   r = csilk_io_tcp_init(loop, out_handle);
        if (r < 0) {
            close(fd);
            return -1;
        }
        out_handle->data = data;
        r = csilk_io_tcp_open(out_handle, (csilk_io_os_sock_t)fd);
        if (r < 0) {
            close(fd);
            return -1;
        }
#ifndef CSILK_USE_URING
        out_handle->flags |= UV_HANDLE_BOUND;
#endif
        return csilk_io_listen((csilk_io_stream_t*)out_handle, backlog, on_new_connection);
    }
#endif
    void* data = out_handle->data;
    int   r = csilk_io_tcp_init(loop, out_handle);
    if (r < 0) {
        return -1;
    }
    out_handle->data = data;
    struct sockaddr_in addr;
    csilk_io_ip4_addr("0.0.0.0", port, &addr);
    r = csilk_io_tcp_bind(out_handle, (const struct sockaddr*)&addr, 0);
    if (r < 0) {
        return -1;
    }
    return csilk_io_listen((csilk_io_stream_t*)out_handle, backlog, on_new_connection);
}

/* --- Worker thread --- */

/** @brief Worker thread entry point for multi-threaded SO_REUSEPORT mode. */
void
worker_thread(void* arg)
{
    worker_data_t*   data = (worker_data_t*)arg;
    worker_pool_t*   wp = data->wp;
    csilk_server_t*  server = wp->server;
    int              port = data->port;
    csilk_barrier_t* barrier = data->barrier;

    _csilk_worker_set_current_pool(wp);
    pin_thread_to_core(wp->worker_index);

    csilk_io_loop_t* loop_ptr = &wp->loop;

#ifdef __APPLE__
    setenv("UV_KQUEUE_OOB", "1", 0);
#endif

    csilk_io_loop_init(loop_ptr);

    wp->loop_ptr = loop_ptr;
    wp->server_handle.data = wp;

    _csilk_worker_init_arena_pool(wp);
    _csilk_worker_init_read_buf_pool(wp);
    _csilk_worker_init_dispatch(wp, loop_ptr);

    int bind_res = bind_and_listen(
        loop_ptr, &wp->server_handle, port, server->config.listen_backlog, true, wp->worker_index);
    if (bind_res < 0) {
        CSILK_LOG_E("Server: worker %d failed to bind and listen", wp->worker_index);
        atomic_store_explicit(&data->success, 0, memory_order_relaxed);
        if (barrier) {
            csilk_barrier_wait(barrier);
        }
        csilk_io_loop_close(loop_ptr);
        return;
    }

    worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
    csilk_io_async_init(loop_ptr, &wp->stop_async, on_worker_stop_async);
    wp->stop_async.data = &sd;

    atomic_store_explicit(&data->success, 1, memory_order_release);
    if (barrier) {
        csilk_barrier_wait(barrier);
    }

    csilk_io_run(loop_ptr, CSILK_IO_RUN_DEFAULT);
    for (int i = 0; i < 16 && csilk_io_loop_alive(loop_ptr); i++) {
        csilk_io_run(loop_ptr, CSILK_IO_RUN_NOWAIT);
    }
    csilk_arena_flush_free_list();
    csilk_body_pool_cleanup();
    csilk_io_loop_close(loop_ptr);
}
