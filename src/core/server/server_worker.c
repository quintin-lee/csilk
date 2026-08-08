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

static void
on_dispatch_async(uv_async_t* handle)
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
        free(task);
        node = csilk_lfq_dequeue(&wp->dispatch_queue);
    }
}

void
_csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop)
{
    csilk_lfq_init(&wp->dispatch_queue);
    uv_async_init(loop, &wp->dispatch_async, on_dispatch_async);
    wp->dispatch_async.data = wp;
}

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

    csilk_dispatch_task_t* task = malloc(sizeof(csilk_dispatch_task_t));
    if (!task) {
        return;
    }
    task->cb = cb;
    task->arg = arg;
    csilk_lfq_enqueue(&wp->dispatch_queue, &task->lfq_node);

    uv_async_send(&wp->dispatch_async);
}

/* --- CPU pinning --- */

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
        uv_thread_t tid = uv_thread_self();
        uv_thread_setaffinity(&tid, cpuset, NULL, 128);
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
                uv_tcp_t*        out_handle,
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
        uv_ip4_addr("0.0.0.0", port, &addr);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
        if (listen(fd, backlog) < 0) {
            close(fd);
            return -1;
        }
        int r = uv_tcp_init(loop, out_handle);
        if (r < 0) {
            close(fd);
            return -1;
        }
        r = uv_tcp_open(out_handle, (uv_os_sock_t)fd);
        if (r < 0) {
            close(fd);
            return -1;
        }
        out_handle->flags |= UV_HANDLE_BOUND;
        return uv_listen((csilk_io_stream_t*)out_handle, backlog, on_new_connection);
    }
#endif
    int r = uv_tcp_init(loop, out_handle);
    if (r < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    r = uv_tcp_bind(out_handle, (const struct sockaddr*)&addr, 0);
    if (r < 0) {
        return -1;
    }
    return uv_listen((csilk_io_stream_t*)out_handle, backlog, on_new_connection);
}

/* --- Worker thread --- */

/** @brief Worker thread entry point for multi-threaded SO_REUSEPORT mode. */
void
worker_thread(void* arg)
{
    worker_data_t*  data = (worker_data_t*)arg;
    worker_pool_t*  wp = data->wp;
    csilk_server_t* server = wp->server;
    int             port = data->port;
    uv_barrier_t*   barrier = data->barrier;
    free(data);

    pin_thread_to_core(wp->worker_index);

    csilk_io_loop_t* loop_ptr = &wp->loop;

#ifdef __APPLE__
    setenv("UV_KQUEUE_OOB", "1", 0);
#endif

    uv_loop_init(loop_ptr);

    wp->loop_ptr = loop_ptr;
    wp->server_handle.data = wp;

    _csilk_worker_init_arena_pool(wp);
    _csilk_worker_init_dispatch(wp, loop_ptr);

    if (bind_and_listen(loop_ptr,
                        &wp->server_handle,
                        port,
                        server->config.listen_backlog,
                        true,
                        wp->worker_index) < 0) {
        if (barrier) {
            uv_barrier_wait(barrier);
        }
        uv_loop_close(loop_ptr);
        return;
    }

    worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
    wp->stop_async.data = &sd;
    uv_async_init(loop_ptr, &wp->stop_async, on_worker_stop_async);

    if (barrier) {
        uv_barrier_wait(barrier);
    }

    csilk_io_run(loop_ptr, CSILK_IO_RUN_DEFAULT);
    uv_loop_close(loop_ptr);
}
