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
#include <pthread.h>

/** @brief Maximum pending work items in the queue. */
#define URING_TP_MAX_WORK 4096

/** @brief A single work or completion entry. */
typedef struct {
	csilk_io_work_t* work;		 /**< The user work handle. */
	csilk_io_work_cb work_cb;	 /**< Work callback (runs on pool thread). */
	csilk_io_after_work_cb after_cb; /**< After-work callback (runs on event loop). */
	int status;			 /**< Result status passed to after_cb. */
} uring_tp_entry_t;

/** @brief Thread pool state. */
struct uring_thread_pool_s {
	int thread_count;      /**< Number of worker threads. */
	volatile bool running; /**< Set to false during shutdown. */
	pthread_t* threads;    /**< Worker thread IDs. */

	/* Work queue — single producer (event loop) → multiple consumers (threads). */
	uring_tp_entry_t queue[URING_TP_MAX_WORK];
	volatile int queue_head;
	volatile int queue_tail;
	pthread_mutex_t queue_mutex;
	pthread_cond_t queue_cond;

	/* Completion queue — multiple producers (threads) → single consumer (event loop). */
	uring_tp_entry_t done[URING_TP_MAX_WORK];
	volatile int done_head;
	volatile int done_tail;
	pthread_mutex_t done_mutex;

	int wakeup_fd; /**< eventfd — signalled when work completes. */
};

/* ---- Thread-local pointer set by the event loop thread ---- */
static _Thread_local uring_thread_pool_t* tls_current_tp = NULL;

void
uring_tp_set_current(uring_thread_pool_t* tp)
{
	tls_current_tp = tp;
}

static void*
worker_routine(void* arg)
{
	uring_thread_pool_t* tp = (uring_thread_pool_t*)arg;

	while (tp->running) {
		pthread_mutex_lock(&tp->queue_mutex);
		while (tp->queue_head == tp->queue_tail && tp->running) {
			pthread_cond_wait(&tp->queue_cond, &tp->queue_mutex);
		}
		if (!tp->running) {
			pthread_mutex_unlock(&tp->queue_mutex);
			break;
		}

		/* Dequeue one item. */
		int idx = tp->queue_head % URING_TP_MAX_WORK;
		uring_tp_entry_t entry = tp->queue[idx];
		tp->queue_head++;
		pthread_mutex_unlock(&tp->queue_mutex);

		/* Execute the work callback. */
		entry.status = 0;
		if (entry.work_cb && entry.work) {
			entry.work_cb(entry.work);
		}

		/* Push to the completion queue. */
		pthread_mutex_lock(&tp->done_mutex);
		int done_idx = tp->done_tail % URING_TP_MAX_WORK;
		tp->done[done_idx] = entry;
		tp->done_tail++;
		pthread_mutex_unlock(&tp->done_mutex);

		/* Wake the event loop. */
		uint64_t val = 1;
		ssize_t w = write(tp->wakeup_fd, &val, sizeof(val));
		(void)w;
	}

	return NULL;
}

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
	tp->queue_head = 0;
	tp->queue_tail = 0;
	tp->done_head = 0;
	tp->done_tail = 0;

	pthread_mutex_init(&tp->queue_mutex, NULL);
	pthread_cond_init(&tp->queue_cond, NULL);
	pthread_mutex_init(&tp->done_mutex, NULL);

	if (nthreads <= 0) {
		nthreads = 1;
	}
	tp->thread_count = nthreads;
	tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
	if (!tp->threads) {
		close(tp->wakeup_fd);
		pthread_mutex_destroy(&tp->queue_mutex);
		pthread_cond_destroy(&tp->queue_cond);
		pthread_mutex_destroy(&tp->done_mutex);
		free(tp);
		return NULL;
	}

	for (int i = 0; i < nthreads; i++) {
		pthread_create(&tp->threads[i], NULL, worker_routine, tp);
	}

	return tp;
}

void
uring_tp_destroy(uring_thread_pool_t* tp)
{
	if (!tp) {
		return;
	}

	tp->running = false;

	/* Wake all workers so they exit the cond_wait loop. */
	pthread_mutex_lock(&tp->queue_mutex);
	pthread_cond_broadcast(&tp->queue_cond);
	pthread_mutex_unlock(&tp->queue_mutex);

	for (int i = 0; i < tp->thread_count; i++) {
		pthread_join(tp->threads[i], NULL);
	}

	/* Drain any remaining completions — after_work_cb is responsible for
	 * freeing or recycling the work handles. */
	uring_tp_drain(tp);

	close(tp->wakeup_fd);
	pthread_mutex_destroy(&tp->queue_mutex);
	pthread_cond_destroy(&tp->queue_cond);
	pthread_mutex_destroy(&tp->done_mutex);
	free(tp->threads);
	free(tp);
}

int
uring_tp_enqueue(uring_thread_pool_t* tp,
		 csilk_io_work_t* work,
		 csilk_io_work_cb work_cb,
		 csilk_io_after_work_cb after_cb)
{
	if (!tp || !work_cb) {
		return -1;
	}

	pthread_mutex_lock(&tp->queue_mutex);
	int count = tp->queue_tail - tp->queue_head;
	if (count >= URING_TP_MAX_WORK) {
		pthread_mutex_unlock(&tp->queue_mutex);
		return -1; /* Queue full. */
	}

	int idx = tp->queue_tail % URING_TP_MAX_WORK;
	tp->queue[idx].work = work;
	tp->queue[idx].work_cb = work_cb;
	tp->queue[idx].after_cb = after_cb;
	tp->queue[idx].status = 0;
	tp->queue_tail++;

	pthread_cond_signal(&tp->queue_cond);
	pthread_mutex_unlock(&tp->queue_mutex);

	return 0;
}

void
uring_tp_drain(uring_thread_pool_t* tp)
{
	if (!tp) {
		return;
	}

	pthread_mutex_lock(&tp->done_mutex);
	while (tp->done_head != tp->done_tail) {
		int idx = tp->done_head % URING_TP_MAX_WORK;
		uring_tp_entry_t entry = tp->done[idx];
		tp->done_head++;
		pthread_mutex_unlock(&tp->done_mutex);

		if (entry.after_cb && entry.work) {
			entry.after_cb(entry.work, entry.status);
		}

		pthread_mutex_lock(&tp->done_mutex);
	}
	pthread_mutex_unlock(&tp->done_mutex);
}

int
uring_tp_wakeup_fd(uring_thread_pool_t* tp)
{
	return tp ? tp->wakeup_fd : -1;
}

/* ---- Integration with csilk_io_queue_work ---- */

int
_csilk_uring_queue_work(csilk_io_work_t* req,
			csilk_io_work_cb work_cb,
			csilk_io_after_work_cb after_cb)
{
	if (tls_current_tp) {
		return uring_tp_enqueue(tls_current_tp, req, work_cb, after_cb);
	}
	/* Fallback: run synchronously if no thread pool is available. */
	work_cb(req);
	after_cb(req, 0);
	return 0;
}
