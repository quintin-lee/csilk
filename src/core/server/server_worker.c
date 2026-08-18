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

/* --- Dispatch --- */

/** @brief Pop a dispatch task from the worker's slab.
 *
 * @param wp The worker pool (must not be NULL).
 * @return A free task, or NULL if the slab is exhausted. */
static csilk_dispatch_task_t*
pool_get_dispatch_task(worker_pool_t* wp)
{
    if (wp->dispatch_task_slab_count > 0) {
        return &wp->dispatch_task_slab[--wp->dispatch_task_slab_count];
    }
    return NULL;
}

/** @brief Return a dispatch task to the worker's slab.
 *
 * If the slab is full the task is freed instead.
 *
 * @param wp     The worker pool (may be NULL — falls back to free).
 * @param task   Task to return (must not be NULL). */
static void
pool_put_dispatch_task(worker_pool_t* wp, csilk_dispatch_task_t* task)
{
    if (!task || !wp) {
        free(task);
        return;
    }
    if (wp->dispatch_task_slab_count < CSILK_DISPATCH_TASK_SLAB_SIZE) {
        wp->dispatch_task_slab[wp->dispatch_task_slab_count++] = *task;
    } else {
        free(task);
    }
}

/** @brief Pre-allocate the dispatch task slab at worker startup. */
void
_csilk_worker_init_dispatch_task_pool(worker_pool_t* wp)
{
    wp->dispatch_task_slab_count = 0;
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
    if (!wp) {
        return;
    }

    csilk_lfq_node_t* node = csilk_lfq_dequeue(&wp->dispatch_queue);
    while (node) {
        csilk_dispatch_task_t* task = (csilk_dispatch_task_t*)node;
        if (task->cb) {
            task->cb(task->arg);
        }
        pool_put_dispatch_task(wp, task);
        node = csilk_lfq_dequeue(&wp->dispatch_queue);
    }
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
 * @note Allocates a task from the slab (or falls back to malloc), enqueues it on the worker's
 *       lock-free dispatch queue, and signals the async handle to wake the target loop.
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

    csilk_dispatch_task_t* task = pool_get_dispatch_task(wp);
    if (!task) {
        task = malloc(sizeof(csilk_dispatch_task_t));
        if (!task) {
            return;
        }
    }
    task->cb = cb;
    task->arg = arg;
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
    _csilk_worker_init_dispatch_task_pool(wp);
    _csilk_worker_init_dispatch(wp, loop_ptr);

    int bind_res = bind_and_listen(
        loop_ptr, &wp->server_handle, port, server->config.listen_backlog, true, wp->worker_index);
    if (bind_res < 0) {
        CSILK_LOG_E("Server: worker %d failed to bind and listen", wp->worker_index);
        data->success = 0;
        if (barrier) {
            csilk_barrier_wait(barrier);
        }
        csilk_io_loop_close(loop_ptr);
        return;
    }

    worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
    csilk_io_async_init(loop_ptr, &wp->stop_async, on_worker_stop_async);
    wp->stop_async.data = &sd;

    data->success = 1;
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
