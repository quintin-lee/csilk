/**
 * @file uring_event_loop.c
 * @brief io_uring event loop — shutdown, worker threads, dispatch, bind.
 *
 * @copyright MIT License
 */

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <pthread.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#endif

#include "core/ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "core/internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "messaging/mq_internal.h"
#include "uring_internal.h"

/* --- Barrier primitives --- */

/**
 * @brief Initialize a pthread-based barrier for worker synchronization.
 * @param[out] b      Barrier to initialize.
 * @param[in]  count  Number of threads that must reach the barrier.
 * @note Initializes the mutex/cond and resets the count and waiting counter.
 */
void
uring_barrier_init(csilk_barrier_t* b, int count)
{
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = count;
    b->waiting = 0;
}

/**
 * @brief Block until all expected threads reach the barrier.
 * @param[in,out] b Barrier to wait on.
 * @note Increments the waiting count; the last arriver broadcasts, others wait
 *       on the condition variable until the count is reached.
 */
void
uring_barrier_wait(csilk_barrier_t* b)
{
    pthread_mutex_lock(&b->mutex);
    b->waiting++;
    if (b->waiting >= b->count) {
        pthread_cond_broadcast(&b->cond);
    } else {
        while (b->waiting < b->count) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

/**
 * @brief Destroy a barrier, freeing its mutex and condition variable.
 * @param[in,out] b Barrier to destroy.
 */
void
uring_barrier_destroy(csilk_barrier_t* b)
{
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

/* --- Signal handler --- */

/**
 * @brief Signal handler callback that initiates server stop.
 * @param[in] server Server to stop (passed via the signal handler context).
 * @note Simply forwards to csilk_server_stop.
 */
void
on_signal(csilk_server_t* server)
{
    csilk_server_stop(server);
}

/* --- Graceful shutdown --- */

/**
 * @brief Close all active client connections during shutdown.
 * @param[in] server Server whose per-worker active client lists are walked.
 * @param[in] loop   Event loop (currently unused).
 * @return Number of client sockets closed.
 * @note For each active client, sends a WebSocket close (1001) or SSE close as
 *       appropriate, then closes the socket fd and clears the active list.
 */
static int
close_active_clients(csilk_server_t* server, struct io_uring* loop)
{
    int count = 0;
    for (int w = 0; w < server->worker_pool_count; w++) {
        worker_pool_t*  wp = &server->worker_pools[w];
        csilk_client_t* client = wp->active_clients;
        while (client) {
            csilk_client_t* next = client->next;
            if (client->ctx.is_websocket) {
                csilk_ws_close(&client->ctx, 1001, "Server stopping");
            } else if (client->ctx.is_sse) {
                csilk_sse_send(&client->ctx, "close", "Server stopping");
                csilk_sse_close(&client->ctx);
            }
            close(client->handle.fd);
            client = next;
            count++;
        }
        wp->active_clients = NULL;
    }
    return count;
}

/**
 * @brief Async callback that performs graceful server shutdown.
 * @param[in] handle Stop async handle whose data points at the server.
 * @note Triggers server-stop hooks, closes the listener/signal/dispatch fds,
 *       drains and closes active clients, signals worker threads to stop, and
 *       frees the message queue.
 */
void
on_stop_async(csilk_io_async_t* handle)
{
    csilk_server_t* server = (csilk_server_t*)handle->data;
    CSILK_LOG_I("Server: initiating graceful shutdown");

    _csilk_trigger_hooks(server, NULL, CSILK_HOOK_SERVER_STOP);

    CSILK_LOG_D("Server: closing server socket listener");
    close(server->server_handle.fd);

    {
        int n = close_active_clients(server, server->loop);
        if (n > 0) {
            CSILK_LOG_I("Server: closed %d active client connection(s)", n);
        }
    }

    close(server->sig_handle.signal_fd);
    close(server->worker_pools[0].dispatch_async.event_fd);
    close(server->async_handle.event_fd);

    for (int i = 1; i < server->worker_pool_count; i++) {
        CSILK_LOG_D("Server: signaling worker thread %d to stop", i);
        uint64_t val = 1;
        if (write(server->worker_pools[i].stop_async.event_fd, &val, sizeof(val)) < 0) {
            // ignore
        }
    }

    if (server->mq) {
        CSILK_LOG_D("Server: freeing message queue");
        _csilk_mq_free(server->mq);
        server->mq = NULL;
    }
}

/* --- Server stop --- */

/**
 * @brief Request a graceful server shutdown via the stop async handle.
 * @param[in] server Server to stop (validated non-NULL).
 * @note Writes to the async eventfd to wake the loop's on_stop_async handler.
 */
void
csilk_server_stop(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    uint64_t val = 1;
    if (write(server->async_handle.event_fd, &val, sizeof(val)) < 0) {
        // ignore error
    }
}

/* --- Worker stop --- */

/**
 * @brief Per-worker async callback that stops a worker's loop.
 * @param[in] handle Worker stop async handle whose data points at
 *                   uring_worker_stop_data_t (server, loop, listen handle, index).
 * @note Closes the listen socket, drains active clients, and closes the
 *       worker's dispatch and stop event fds.
 */
static void
on_worker_stop_async(csilk_io_async_t* handle)
{
    uring_worker_stop_data_t* sd = (uring_worker_stop_data_t*)handle->data;
    if (!sd) {
        return;
    }

    csilk_server_t*  server = sd->server;
    csilk_io_loop_t* loop = sd->loop;

    close(sd->listen_handle->fd);

    close_active_clients(server, loop);

    int worker_idx = sd->worker_index;
    close(server->worker_pools[worker_idx].dispatch_async.event_fd);
    close(handle->event_fd);
}

/* --- Dispatch --- */

/**
 * @brief io_uring dispatch async handler: drain and run queued dispatch tasks.
 * @param[in] handle Dispatch async handle whose data points at the worker pool.
 * @note Dequeues every csilk_dispatch_task_t from the worker dispatch queue and
 *       runs its callback, freeing each task.
 */
void
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
        free(task);
        node = csilk_lfq_dequeue(&wp->dispatch_queue);
    }
}

/**
 * @brief Initialize the uring worker-pool dispatch queue and async eventfd.
 * @param[in] wp   Worker pool whose dispatch queue is initialized.
 * @param[in] loop Event loop (currently unused by the uring backend).
 * @note Initializes the lock-free dispatch queue and an EFD_NONBLOCK|EFD_CLOEXEC
 *       eventfd for the dispatch async; the async handle's data is set to wp.
 */
void
_csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop)
{
    csilk_lfq_init(&wp->dispatch_queue);
    wp->dispatch_async.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    wp->dispatch_async.data = wp;
}

/**
 * @brief Queue a callback to run on the owning worker's uring dispatch loop.
 * @param[in] c   Request context whose internal client identifies the worker.
 * @param[in] cb  Callback to invoke with arg on the worker loop.
 * @param[in] arg Argument passed to cb.
 * @note No-op if c, its internal client, the client's owner pool, or cb is NULL.
 *       Allocates a task, enqueues it on the worker dispatch queue, and writes
 *       to the dispatch eventfd to wake the worker.
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

    csilk_dispatch_task_t* task = malloc(sizeof(csilk_dispatch_task_t));
    if (!task) {
        return;
    }
    task->cb = cb;
    task->arg = arg;
    csilk_lfq_enqueue(&wp->dispatch_queue, &task->lfq_node);

    uint64_t val = 1;
    if (write(wp->dispatch_async.event_fd, &val, sizeof(val)) < 0) {
        // ignore
    }
}

/* --- Bind and listen --- */

/**
 * @brief Create, bind, and listen on a non-blocking TCP socket for the ring.
 * @param[in]  loop       Event loop (currently unused).
 * @param[out] out_handle Receives the created socket fd and a NULL data pointer.
 * @param[in]  port      Port to bind to (INADDR_ANY).
 * @param[in]  backlog   listen(2) backlog.
 * @param[in]  reuseport If true, set SO_REUSEADDR and SO_REUSEPORT.
 * @return The socket fd on success, or -1 on socket/bind/listen failure (or on
 *         Windows, where this is unsupported).
 * @note On non-Apple platforms the socket is created SOCK_NONBLOCK|SOCK_CLOEXEC.
 *       On failure any partially created fd is closed before returning.
 */
int
uring_bind_and_listen(
    csilk_io_loop_t* loop, csilk_io_tcp_t* out_handle, int port, int backlog, bool reuseport)
{
    (void)loop;
#ifndef _WIN32
    int fd;
    if (reuseport) {
#ifdef __APPLE__
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
#else
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    } else {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    out_handle->fd = fd;
    out_handle->data = NULL;
    return fd;
#else
    return -1;
#endif
}

/* --- Worker thread --- */

/**
 * @brief Entry point for a spawned io_uring worker thread.
 * @param[in] arg Pointer to an uring_worker_data_t (freed on entry) describing
 *                the worker pool, server, port, and startup barrier.
 * @return NULL on thread completion.
 * @note Initializes the worker's io_uring ring (SQPOLL, falling back to
 *       polling), arena and dispatch queue, a thread pool, binds the listening
 *       socket, then runs the per-worker event loop until stopped. Signals the
 *       startup barrier before entering the loop.
 */
void*
uring_worker_thread(void* arg)
{
    uring_worker_data_t* data = (uring_worker_data_t*)arg;
    worker_pool_t*       wp = data->wp;
    csilk_server_t*      server = wp->server;
    int                  port = data->port;
    csilk_barrier_t*     barrier = data->barrier;
    free(data);

    csilk_io_loop_t* loop_ptr = &wp->loop;
    wp->loop_ptr = loop_ptr;
    int uring_flags_worker = IORING_SETUP_SQPOLL;
    if (io_uring_queue_init(4096, loop_ptr, uring_flags_worker) < 0) {
        CSILK_LOG_W("Worker %d: SQPOLL unavailable, falling back to non-polling", wp->worker_index);
        if (io_uring_queue_init(4096, loop_ptr, 0) < 0) {
            CSILK_LOG_E("Worker %d: io_uring_queue_init failed", wp->worker_index);
            if (barrier) {
                uring_barrier_wait(barrier);
            }
            return NULL;
        }
    }

    wp->server_handle.data = wp;

    _csilk_worker_init_arena_pool(wp);
    _csilk_worker_init_dispatch(wp, loop_ptr);

    {
        int nprocs = (int)sysconf(_SC_NPROCESSORS_ONLN);
        int nworkers = server->worker_pool_count;
        int tp_nthreads = (nprocs > 0) ? nprocs / nworkers : 1;
        if (tp_nthreads < 1) {
            tp_nthreads = 1;
        }
        wp->thread_pool = uring_tp_init(tp_nthreads);
        if (wp->thread_pool) {
            uring_tp_set_current((uring_thread_pool_t*)wp->thread_pool);
        }
        CSILK_LOG_I(
            "Worker %d: thread pool initialised (%d threads)", wp->worker_index, tp_nthreads);
    }

    if (uring_bind_and_listen(
            loop_ptr, &wp->server_handle, port, server->config.listen_backlog, true) < 0) {
        if (barrier) {
            uring_barrier_wait(barrier);
        }
        io_uring_queue_exit(loop_ptr);
        return NULL;
    }

    uring_worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
    wp->stop_async.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    wp->stop_async.data = &sd;

    struct io_uring_sqe* stop_sqe = io_uring_get_sqe(loop_ptr);
    io_uring_prep_poll_add(stop_sqe, wp->stop_async.event_fd, POLLIN);
    io_uring_sqe_set_data64(stop_sqe, uring_encode_data(URING_OP_WAKEUP, NULL, &wp->stop_async));

    struct io_uring_sqe* disp_sqe = io_uring_get_sqe(loop_ptr);
    io_uring_prep_poll_add(disp_sqe, wp->dispatch_async.event_fd, POLLIN);
    io_uring_sqe_set_data64(disp_sqe,
                            uring_encode_data(URING_OP_WAKEUP, NULL, &wp->dispatch_async));

    if (wp->thread_pool) {
        int                  tp_fd = uring_tp_wakeup_fd((uring_thread_pool_t*)wp->thread_pool);
        struct io_uring_sqe* tp_sqe = io_uring_get_sqe(loop_ptr);
        if (tp_sqe && tp_fd >= 0) {
            io_uring_prep_poll_add(tp_sqe, tp_fd, POLLIN);
            io_uring_sqe_set_data64(tp_sqe,
                                    uring_encode_data(URING_OP_WAKEUP, NULL, wp->thread_pool));
        }
    }

    struct io_uring_sqe* acc_sqe = io_uring_get_sqe(loop_ptr);
    io_uring_prep_accept(acc_sqe, wp->server_handle.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(acc_sqe, uring_encode_data(URING_OP_ACCEPT, NULL, wp));

    io_uring_submit(loop_ptr);

    if (barrier) {
        uring_barrier_wait(barrier);
    }

    struct io_uring_cqe* cqe;
    int                  running = 1;
    while (running) {
        int ret = io_uring_wait_cqe(loop_ptr, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            CSILK_LOG_E("io_uring_wait_cqe failed: %d", ret);
            break;
        }

        int             res = cqe->res;
        int             flags = cqe->flags;
        uring_op_type_t op;
        void*           ptr;
        uint8_t         gen = 0;
        uring_decode_data(io_uring_cqe_get_data64(cqe), &op, &ptr, &gen);
        if (ptr && op != URING_OP_ACCEPT && op != URING_OP_WAKEUP && op != URING_OP_CLOSE &&
            op != URING_OP_TMR_GENERIC) {
            csilk_client_t* client = (csilk_client_t*)ptr;
            if (op == URING_OP_WRITE) {
                client = ((uring_write_req_t*)ptr)->client;
            } else if (op == URING_OP_UV_WRITE) {
                client = (csilk_client_t*)(((void**)ptr)[0]);
            }
            if (client->generation != gen) {
                CSILK_LOG_D("Worker: ignoring old CQE (op %d, res %d)", op, res);
                io_uring_cqe_seen(loop_ptr, cqe);
                continue;
            }
        }

        io_uring_cqe_seen(loop_ptr, cqe);

        CSILK_LOG_D("Worker %d: wait_cqe returned op %d, res %d, flags %d",
                    wp->worker_index,
                    op,
                    res,
                    flags);

        if (op == URING_OP_ACCEPT) {
            if (res >= 0) {
                on_new_connection((worker_pool_t*)ptr, res);
            } else if (res != -EAGAIN && res != -ECANCELED) {
                CSILK_LOG_E("Accept failed with %d", res);
            }
            acc_sqe = io_uring_get_sqe(loop_ptr);
            if (acc_sqe) {
                io_uring_prep_accept(
                    acc_sqe, wp->server_handle.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                io_uring_sqe_set_data64(acc_sqe, uring_encode_data(URING_OP_ACCEPT, NULL, wp));
                int submit_ret = io_uring_submit(loop_ptr);
                if (submit_ret < 0) {
                    CSILK_LOG_E("io_uring_submit failed: %d", submit_ret);
                }
            } else {
                CSILK_LOG_E("Failed to get SQE for accept!");
            }
        } else if (op == URING_OP_WAKEUP) {
            if (ptr == &wp->dispatch_async) {
                uint64_t val;
                if (read(wp->dispatch_async.event_fd, &val, sizeof(val)) > 0) {
                    on_dispatch_async(&wp->dispatch_async);
                }
                struct io_uring_sqe* poll_sqe = io_uring_get_sqe(loop_ptr);
                if (poll_sqe) {
                    io_uring_prep_poll_add(poll_sqe, wp->dispatch_async.event_fd, POLLIN);
                    io_uring_sqe_set_data64(poll_sqe,
                                            uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
                    io_uring_submit(loop_ptr);
                }
            } else if (wp->thread_pool && ptr == wp->thread_pool) {
                uint64_t             val;
                uring_thread_pool_t* tp = (uring_thread_pool_t*)wp->thread_pool;
                if (read(uring_tp_wakeup_fd(tp), &val, sizeof(val)) > 0) {
                    uring_tp_drain(tp);
                }
                int                  tp_fd = uring_tp_wakeup_fd(tp);
                struct io_uring_sqe* poll_sqe = io_uring_get_sqe(loop_ptr);
                if (poll_sqe && tp_fd >= 0) {
                    io_uring_prep_poll_add(poll_sqe, tp_fd, POLLIN);
                    io_uring_sqe_set_data64(poll_sqe,
                                            uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
                    io_uring_submit(loop_ptr);
                }
            } else if (ptr == &wp->stop_async) {
                uint64_t val;
                if (read(wp->stop_async.event_fd, &val, sizeof(val)) > 0) {
                    on_worker_stop_async(&wp->stop_async);
                    running = 0;
                }
            }
        } else if (op == URING_OP_READ) {
            on_read((csilk_client_t*)ptr, cqe->res);
        } else if (op == URING_OP_WRITE) {
            on_write_done(ptr, cqe->res);
        } else if (op == URING_OP_UV_WRITE) {
            csilk_uv_on_write_done(ptr, cqe->res);
        } else if (op == URING_OP_TMR_READ || op == URING_OP_TMR_WRITE || op == URING_OP_TMR_IDLE ||
                   op == URING_OP_TMR_REQ) {
            on_timeout((csilk_client_t*)ptr);
        } else if (op == URING_OP_TMR_GENERIC) {
            csilk_io_timer_t* tmr = (csilk_io_timer_t*)ptr;
            if (tmr && tmr->generation == gen && tmr->cb) {
                tmr->cb(tmr);
            }
        } else if (op == URING_OP_CLOSE) {
            on_close_done((csilk_client_t*)ptr);
        }
    }

    io_uring_queue_exit(loop_ptr);
    return NULL;
}
