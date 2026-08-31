/**
 * @file mq_core.c
 * @brief MQ core lifecycle and statistics — instance creation, teardown,
 * monitoring, and metrics.
 *
 * Implements the core MQ instance management: allocation/free of the queue
 * (bound to an I/O event loop), registration of WebSocket monitors, and
 * collection/serialization of queue statistics.  WAL persistence fields are
 * initialized here and finalized in on_mq_close().
 * @copyright MIT License
 */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/core/internal.h"
#include "mq_internal.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/mq.h"
#include "mq_internal.h"

extern void on_mq_async(csilk_io_async_t* handle);

/**
 * @brief Snapshot current MQ statistics under the queue lock.
 *
 * Copies the published/delivered/failed totals, current queue depth, and the
 * topic count into @p stats.  Either argument may be NULL, in which case the
 * call is a no-op.
 *
 * @param[in]  mq    The MQ instance.
 * @param[out] stats Destination stats struct to populate.
 * @note Access to the counters is serialized by queue_mutex.
 */
void
csilk_mq_get_stats(csilk_mq_t* mq, csilk_mq_stats_t* stats)
{
    if (!mq || !stats) {
        return;
    }
    csilk_mutex_lock(&mq->queue_mutex);
    stats->published_total = mq->published_total;
    stats->delivered_total = mq->delivered_total;
    stats->failed_total = mq->failed_total;
    stats->queue_depth = mq->queue_depth;

    uint32_t topics = 0;
    for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
        topics++;
    }
    stats->topic_count = topics;
    csilk_mutex_unlock(&mq->queue_mutex);
}

/**
 * @brief Serialize MQ statistics into a pretty-printed JSON string.
 *
 * Builds a JSON object with the published/delivered/failed totals, queue
 * depth, and topic count, then pretty-serializes it.
 *
 * @param[in] stats Pointer to the stats struct (may be NULL — returns NULL).
 * @return Heap-allocated JSON string (caller must free), or NULL on error.
 */
char*
csilk_mq_stats_to_json(const csilk_mq_stats_t* stats)
{
    if (!stats) {
        return NULL;
    }
    csilk_json_t* root = csilk_json_object();
    csilk_json_add_number(root, "published_total", (double)stats->published_total);
    csilk_json_add_number(root, "delivered_total", (double)stats->delivered_total);
    csilk_json_add_number(root, "failed_total", (double)stats->failed_total);
    csilk_json_add_number(root, "queue_depth", (double)stats->queue_depth);
    csilk_json_add_number(root, "topic_count", (double)stats->topic_count);
    char* json = csilk_json_serialize_pretty(root, NULL);
    csilk_json_free(root);
    return json;
}

/**
 * @brief Register a WebSocket monitor for live MQ events.
 *
 * Appends the framework context to the monitor array, growing the array
 * (doubling capacity, starting at 4) as needed under monitor_mutex.
 *
 * @param[in] mq The MQ instance (must not be NULL).
 * @param[in] c  Framework context for the monitor connection (must not be NULL).
 * @note Logs an error and returns without registering if either argument is
 *       NULL, or reallocation fails.
 */
void
csilk_mq_register_monitor(csilk_mq_t* mq, csilk_ctx_t* c)
{
    if (!mq || !c) {
        CSILK_LOG_E("MQ: Failed to register monitor: invalid arguments");
        return;
    }
    csilk_mutex_lock(&mq->monitor_mutex);
    if (mq->monitor_count >= mq->monitor_capacity) {
        size_t new_cap;
        if (mq->monitor_capacity == 0) {
            new_cap = 4;
        } else {
            if (mq->monitor_capacity > SIZE_MAX / 2) {
                csilk_mutex_unlock(&mq->monitor_mutex);
                return;
            }
            new_cap = mq->monitor_capacity * 2;
        }
        if (new_cap > SIZE_MAX / sizeof(csilk_ctx_t*)) {
            csilk_mutex_unlock(&mq->monitor_mutex);
            return;
        }
        csilk_ctx_t** new_monitors = realloc(mq->monitors, new_cap * sizeof(csilk_ctx_t*));
        if (!new_monitors) {
            CSILK_LOG_E("MQ: Failed to allocate memory for monitor registration");
            csilk_mutex_unlock(&mq->monitor_mutex);
            return;
        }
        mq->monitors = new_monitors;
        mq->monitor_capacity = new_cap;
    }
    mq->monitors[mq->monitor_count++] = c;
    CSILK_LOG_I("MQ: Monitor %p registered. Total monitors: %zu", (void*)c, mq->monitor_count);
    csilk_mutex_unlock(&mq->monitor_mutex);
}

/**
 * @brief Unregister a WebSocket monitor from MQ events.
 */
void
csilk_mq_unregister_monitor(csilk_mq_t* mq, csilk_ctx_t* c)
{
    if (!mq || !c) {
        return;
    }
    csilk_mutex_lock(&mq->monitor_mutex);
    for (size_t i = 0; i < mq->monitor_count; i++) {
        if (mq->monitors[i] == c) {
            for (size_t j = i; j + 1 < mq->monitor_count; j++) {
                mq->monitors[j] = mq->monitors[j + 1];
            }
            mq->monitor_count--;
            CSILK_LOG_I(
                "MQ: Monitor %p unregistered. Total monitors: %zu", (void*)c, mq->monitor_count);
            break;
        }
    }
    csilk_mutex_unlock(&mq->monitor_mutex);
}

/**
 * @brief Internal: Create a new MQ instance bound to an I/O event loop.
 *
 * Allocates and zero-initializes the queue, initializes the queue/monitor/WAL
 * mutexes, and registers the async handle used to bridge worker-thread
 * publishes into the main loop.  WAL is disabled (wal_fd = -1) until
 * csilk_mq_set_persistence is called.
 *
 * @param[in] loop The I/O event loop (libuv or io_uring).
 * @return A new MQ instance (heap-allocated), or NULL on allocation failure.
 */
CSILK_INTERNAL csilk_mq_t*
_csilk_mq_new(csilk_io_loop_t* loop)
{
    csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
    if (!mq) {
        CSILK_LOG_E("MQ: Failed to allocate memory for message queue");
        return NULL;
    }

    csilk_mutex_init(&mq->queue_mutex);
    csilk_mutex_init(&mq->monitor_mutex);
    mq->loop = loop;

    csilk_io_async_init(loop, &mq->async_handle, on_mq_async);
    mq->async_handle.data = mq;

    mq->wal_fd = -1;
    mq->wal_path = NULL;
    csilk_mutex_init(&mq->wal_mutex);

    CSILK_LOG_I("MQ: Message queue initialized successfully");
    return mq;
}

/* Releases all MQ resources: flushes the WAL, closes the WAL fd, frees the
 * pending message queue, the topic registry, the global middleware array, and
 * finally the instance.  Invoked by csilk_io_close once the async handle is
 * fully closed. */
static void
on_mq_close(csilk_io_handle_t* handle)
{
    csilk_mq_t* mq = (csilk_mq_t*)handle->data;
    if (!mq) {
        return;
    }

    _mq_wal_flush();

    csilk_mutex_destroy(&mq->queue_mutex);
    csilk_mutex_destroy(&mq->wal_mutex);

    if (mq->wal_fd >= 0) {
        csilk_io_fs_t close_req;
        csilk_io_fs_close(handle->loop, &close_req, mq->wal_fd, NULL);
        csilk_io_fs_req_cleanup(&close_req);
    }
    if (mq->wal_path) {
        free(mq->wal_path);
    }

    csilk_mq_msg_t* msg = mq->queue_head;
    while (msg) {
        csilk_mq_msg_t* next = msg->next;
        free(msg->topic);
        free(msg->payload);
        free(msg);
        msg = next;
    }

    csilk_mq_topic_t* topic = mq->topics;
    while (topic) {
        csilk_mq_topic_t* next = topic->next;
        free(topic->name);
        free(topic->handlers);
        free(topic);
        topic = next;
    }

    free(mq->global_middlewares);
    free(mq->monitors);

    CSILK_LOG_I("MQ: Message queue closed and resource cleanup complete");
    free(mq);
}

/**
 * @brief Internal: Destroy an MQ instance asynchronously.
 *
 * If the async handle is not already closing, schedules on_mq_close via
 * csilk_io_close; the actual resource teardown happens in that callback on
 * the event loop.
 *
 * @param[in] mq The MQ instance to free (may be NULL — no-op).
 */
CSILK_INTERNAL void
_csilk_mq_free(csilk_mq_t* mq)
{
    if (!mq) {
        return;
    }
    if (!csilk_io_is_closing((csilk_io_handle_t*)&mq->async_handle)) {
        mq->async_handle.data = mq;
        csilk_io_close((csilk_io_handle_t*)&mq->async_handle, on_mq_close);
    }
}
