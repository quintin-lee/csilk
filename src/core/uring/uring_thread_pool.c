/**
 * @file uring_thread_pool.c
 * @brief Thread pool for blocking I/O operations in io_uring mode.
 *
 * Provides an async work queue backed by N worker threads.  The event loop
 * enqueues work items; a worker thread executes them; on completion the
 * item is pushed to a done queue and the event loop is signalled via an
 * eventfd.  The event loop calls uring_tp_drain() to invoke after-callbacks.
 *
 * @copyright MIT License
 */

#include "uring_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include "csilk/core/sync.h"

/** @brief Maximum pending work items in the queue. */
#define URING_TP_MAX_WORK 4096

/** @brief A single work or completion entry. */
typedef struct {
    csilk_io_work_t*       work;     /**< The user work handle. */
    csilk_io_work_cb       work_cb;  /**< Work callback (runs on pool thread). */
    csilk_io_after_work_cb after_cb; /**< After-work callback (runs on event loop). */
    int                    status;   /**< Result status passed to after_cb. */
} uring_tp_entry_t;

/** @brief Thread pool state. */
struct uring_thread_pool_s {
    int           thread_count; /**< Number of worker threads. */
    volatile bool running;      /**< Set to false during shutdown. */
    pthread_t*    threads;      /**< Worker thread IDs. */

    /* Work queue — single producer (event loop) → multiple consumers (threads). */
    uring_tp_entry_t queue[URING_TP_MAX_WORK];
    atomic_int       queue_head;
    atomic_int       queue_tail;
    csilk_mutex_t    queue_mutex;
    csilk_cond_t     queue_cond;

    /* Completion queue — multiple producers (threads) → single consumer (event loop). */
    uring_tp_entry_t done[URING_TP_MAX_WORK];
    atomic_int       done_head;
    atomic_int       done_tail;
    csilk_mutex_t    done_mutex;

    int wakeup_fd; /**< eventfd — signalled when work completes. */
};

/* ---- Thread-local pointer set by the event loop thread ---- */
static _Thread_local uring_thread_pool_t* tls_current_tp = NULL;

/**
 * @brief Set the thread-local "current" thread pool for the calling thread.
 * @param[in] tp Thread pool to record in thread-local storage (may be NULL).
 * @note Used so csilk_io_queue_work can find the pool installed by the event
 *       loop without passing it through every call.
 */
void
uring_tp_set_current(uring_thread_pool_t* tp)
{
    tls_current_tp = tp;
}

/**
 * @brief Worker thread routine: dequeue, run, and complete work items.
 * @param[in] arg Thread pool pointer.
 * @return NULL when the pool is stopped.
 * @note Loops while tp->running: waits on the queue condvar, runs the work
 *       callback on a dequeued entry, pushes the result to the done queue, and
 *       writes to the wakeup eventfd to notify the event loop.
 */
static void*
worker_routine(void* arg)
{
    uring_thread_pool_t* tp = (uring_thread_pool_t*)arg;

    while (tp->running) {
        csilk_mutex_lock(&tp->queue_mutex);
        while (atomic_load(&tp->queue_head) == tp->queue_tail && tp->running) {
            csilk_cond_wait(&tp->queue_cond, &tp->queue_mutex);
        }
        if (!tp->running) {
            csilk_mutex_unlock(&tp->queue_mutex);
            break;
        }

        /* Dequeue one item. */
        int              idx = atomic_load(&tp->queue_head) % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->queue[idx];
        atomic_fetch_add(&tp->queue_head, 1);
        csilk_mutex_unlock(&tp->queue_mutex);

        /* Execute the work callback. */
        entry.status = 0;
        if (entry.work_cb && entry.work) {
            entry.work_cb(entry.work);
        }

        /* Push to the completion queue. */
        csilk_mutex_lock(&tp->done_mutex);
        int done_idx = atomic_load(&tp->done_tail) % URING_TP_MAX_WORK;
        tp->done[done_idx] = entry;
        atomic_fetch_add(&tp->done_tail, 1);
        csilk_mutex_unlock(&tp->done_mutex);

        /* Wake the event loop. */
        uint64_t val = 1;
        ssize_t  w = write(tp->wakeup_fd, &val, sizeof(val));
        (void)w;
    }

    return NULL;
}

/**
 * @brief Create and start a thread pool with the given number of workers.
 * @param[in] nthreads Desired worker count (clamped to >= 1).
 * @return A newly allocated, running pool, or NULL on allocation or eventfd
 *         failure.
 * @note Creates a non-blocking/cloexec wakeup eventfd, initializes the work and
 *       done queues and their mutex/cond, and spawns the worker threads which
 *       immediately begin consuming the work queue.
 */
uring_thread_pool_t*
uring_tp_init(int nthreads)
{
    uring_thread_pool_t* tp = calloc(1, sizeof(uring_thread_pool_t));
    if (!tp) {
        return NULL;
    }

    tp->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (tp->wakeup_fd < 0) {
        free(tp);
        return NULL;
    }

    tp->running = true;
    atomic_store(&tp->queue_head, 0);
    atomic_store(&tp->queue_tail, 0);
    atomic_store(&tp->done_head, 0);
    atomic_store(&tp->done_tail, 0);

    csilk_mutex_init(&tp->queue_mutex);
    csilk_cond_init(&tp->queue_cond);
    csilk_mutex_init(&tp->done_mutex);

    if (nthreads <= 0) {
        nthreads = 1;
    }
    tp->thread_count = nthreads;
    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!tp->threads) {
        close(tp->wakeup_fd);
        csilk_mutex_destroy(&tp->queue_mutex);
        csilk_cond_destroy(&tp->queue_cond);
        csilk_mutex_destroy(&tp->done_mutex);
        free(tp);
        return NULL;
    }

    for (int i = 0; i < nthreads; i++) {
        pthread_create(&tp->threads[i], NULL, worker_routine, tp);
    }

    return tp;
}

/**
 * @brief Stop and tear down a thread pool.
 * @param[in] tp Pool to destroy (no-op if NULL).
 * @note Sets running=false, broadcasts the queue condvar to wake workers, joins
 *       all threads, drains remaining completions (so after-callbacks free work
 *       handles), then closes the wakeup fd and destroys the mutex/cond.
 */
void
uring_tp_destroy(uring_thread_pool_t* tp)
{
    if (!tp) {
        return;
    }

    tp->running = false;

    /* Wake all workers so they exit the cond_wait loop. */
    csilk_mutex_lock(&tp->queue_mutex);
    csilk_cond_broadcast(&tp->queue_cond);
    csilk_mutex_unlock(&tp->queue_mutex);

    for (int i = 0; i < tp->thread_count; i++) {
        pthread_join(tp->threads[i], NULL);
    }

    /* Drain any remaining completions — after_work_cb is responsible for
     * freeing or recycling the work handles. */
    uring_tp_drain(tp);

    close(tp->wakeup_fd);
    csilk_mutex_destroy(&tp->queue_mutex);
    csilk_cond_destroy(&tp->queue_cond);
    csilk_mutex_destroy(&tp->done_mutex);
    free(tp->threads);
    free(tp);
}

/**
 * @brief Enqueue a work item onto the thread pool's bounded queue.
 * @param[in] tp      Thread pool to enqueue into (validated non-NULL).
 * @param[in] work    Opaque work object passed to the callbacks.
 * @param[in] work_cb Callback executed on a worker thread.
 * @param[in] after_cb Callback executed on the done path after work completes.
 * @return 0 on success, -1 on NULL tp/work_cb or when the queue is full.
 * @note Thread-safe: takes the pool queue mutex and signals a worker via
 *       pthread_cond. The internal ring buffer is bounded by URING_TP_MAX_WORK;
 *       a full queue is rejected rather than blocking.
 */
int
uring_tp_enqueue(uring_thread_pool_t*   tp,
                 csilk_io_work_t*       work,
                 csilk_io_work_cb       work_cb,
                 csilk_io_after_work_cb after_cb)
{
    if (!tp || !work_cb) {
        return -1;
    }

    int count = atomic_load(&tp->queue_tail) - atomic_load(&tp->queue_head);
    if (count >= URING_TP_MAX_WORK) {
        csilk_mutex_unlock(&tp->queue_mutex);
        return -1; /* Queue full. */
    }
    int idx = atomic_load(&tp->queue_tail) % URING_TP_MAX_WORK;
    tp->queue[idx].work = work;
    tp->queue[idx].work_cb = work_cb;
    tp->queue[idx].after_cb = after_cb;
    tp->queue[idx].status = 0;
    atomic_fetch_add(&tp->queue_tail, 1);

    pthread_cond_signal(&tp->queue_cond);
    csilk_mutex_unlock(&tp->queue_mutex);

    return 0;
}

/**
 * @brief Drain the completion queue, invoking each after-work callback.
 * @param[in] tp Pool whose done queue is drained (no-op if NULL).
 * @note Iterates the done ring under the done mutex, calling after_cb(work, status)
 *       for each entry. Safe to call from the event loop after being woken via
 *       the wakeup eventfd.
 */
void
uring_tp_drain(uring_thread_pool_t* tp)
{
    if (!tp) {
        return;
    }

    while (atomic_load(&tp->done_head) != tp->done_tail) {
        int              idx = atomic_load(&tp->done_head) % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->done[idx];
        atomic_fetch_add(&tp->done_head, 1);
        csilk_mutex_unlock(&tp->done_mutex);

        if (entry.after_cb && entry.work) {
            entry.after_cb(entry.work, entry.status);
        }

        csilk_mutex_lock(&tp->done_mutex);
    }
    csilk_mutex_unlock(&tp->done_mutex);
}

/**
 * @brief Return the wakeup eventfd of the thread pool.
 * @param[in] tp Pool to query (may be NULL).
 * @return The pool's wakeup eventfd, or -1 if tp is NULL.
 * @note The event loop reads this fd to learn when completions are ready.
 */
int
uring_tp_wakeup_fd(uring_thread_pool_t* tp)
{
    return tp ? tp->wakeup_fd : -1;
}

/* ---- Deferred callback queue (sync-fallback path) ---- */
/* When no thread pool is available, _csilk_uring_queue_work runs the work
 * callback inline but defers the after-work callback so that it fires during
 * csilk_io_run(). This preserves the async contract expected by subsystems
 * such as the workflow scheduler that use csilk_io_queue_work.
 *
 * The queue is thread-local — no synchronisation needed. */
static _Thread_local uring_deferred_t* tls_deferred_head = NULL;
static _Thread_local uring_deferred_t* tls_deferred_tail = NULL;

/**
 * @brief Push a deferred after-work callback onto the thread-local queue.
 * @param[in] work    Work handle passed to after_cb.
 * @param[in] after_cb Deferred callback (may be NULL).
 * @param[in] status  Status value delivered to after_cb.
 * @note Used by the synchronous fallback path so after_cb runs later during
 *       csilk_io_run(). The queue is thread-local and needs no locking.
 */
void
_uring_deferred_push(csilk_io_work_t* work, csilk_io_after_work_cb after_cb, int status)
{
    uring_deferred_t* d = (uring_deferred_t*)malloc(sizeof(uring_deferred_t));
    if (!d) {
        return;
    }
    d->work = work;
    d->after_cb = after_cb;
    d->status = status;
    d->next = NULL;

    if (tls_deferred_tail) {
        tls_deferred_tail->next = d;
    } else {
        tls_deferred_head = d;
    }
    tls_deferred_tail = d;
}

/**
 * @brief Drain and run all thread-local deferred after-work callbacks.
 * @return Number of deferred callbacks invoked.
 * @note Walks the thread-local deferred list (reset to empty first), invoking
 *       after_cb(work, status) for each entry and freeing the node.
 */
int
_uring_deferred_drain_all(void)
{
    int               count = 0;
    uring_deferred_t* d = tls_deferred_head;
    tls_deferred_head = NULL;
    tls_deferred_tail = NULL;

    while (d) {
        uring_deferred_t* next = d->next;
        if (d->after_cb && d->work) {
            d->after_cb(d->work, d->status);
        }
        free(d);
        d = next;
        count++;
    }
    return count;
}

/* ---- Integration with csilk_io_queue_work ---- */

/* Depth counter: tracks nested csilk_io_queue_work calls in the synchronous
 * fallback path (no thread pool).  Top-level calls defer after_cb so it runs
 * during csilk_io_run() (required by the workflow scheduler lifecycle).
 * Nested calls (e.g. tool execution inside an AI node) run after_cb
 * immediately to unblock csilk_cond_wait in the caller. */
static _Thread_local int tls_queue_work_depth = 0;

/**
 * @brief Submit work through the uring backend's queue-work abstraction.
 * @param[in] req      Work request passed to the callbacks.
 * @param[in] work_cb  Callback run synchronously (pool) or inline (fallback).
 * @param[in] after_cb Callback for post-work notification.
 * @return 0 on success, or the error from uring_tp_enqueue if the pool is full.
 * @note If a thread pool is installed it delegates to uring_tp_enqueue. In the
 *       synchronous fallback (no pool) work_cb runs inline and after_cb is
 *       deferred to csilk_io_run() at top level, or invoked immediately when
 *       called from a nested csilk_io_queue_work (tracked via a thread-local
 *       depth counter) to avoid deadlocking a waiting caller.
 */
int
_csilk_uring_queue_work(csilk_io_work_t*       req,
                        csilk_io_work_cb       work_cb,
                        csilk_io_after_work_cb after_cb)
{
    if (tls_current_tp) {
        return uring_tp_enqueue(tls_current_tp, req, work_cb, after_cb);
    }

    tls_queue_work_depth++;
    work_cb(req);
    int is_nested = tls_queue_work_depth > 1;
    tls_queue_work_depth--;

    if (is_nested) {
        if (after_cb) {
            after_cb(req, 0);
        }
    } else {
        _uring_deferred_push(req, after_cb, 0);
    }
    return 0;
}
