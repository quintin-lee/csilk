/**
 * @file mq_dispatch.c
 * @brief MQ dispatch and publish path — enqueue, async delivery, monitor
 * broadcast.
 *
 * Implements message publishing (with optional WAL append), the async callback
 * that drains the in-memory queue on the main event loop, and construction of
 * the per-message handler chain (global middleware + matching topic handlers,
 * resolved by fnmatch).  Also broadcasts delivery events to WebSocket monitors.
 * @copyright MIT License
 */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/json/json.h"
#include "csilk/core/internal.h"
#include "../core/internal/srv_impl.h"
#include "mq_internal.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/mq.h"

/* Forward declaration for synchronous MQ dispatch in io_uring backend */
void on_mq_async(csilk_io_async_t* handle);

typedef struct mq_ws_broadcast_task_s {
    csilk_client_t* client;
    csilk_ctx_t*    ctx;
    char*           json;
    size_t          len;
} mq_ws_broadcast_task_t;

static void
_mq_broadcast_dispatch_cb(void* arg)
{
    mq_ws_broadcast_task_t* t = (mq_ws_broadcast_task_t*)arg;
    if (!t) {
        return;
    }
    if (!csilk_ctx_is_closed(t->ctx)) {
        csilk_ws_send(t->ctx, (const uint8_t*)t->json, t->len, 0x1);
    }
    csilk_client_unref(t->client);
    free(t->json);
    free(t);
}

/* Sends a JSON monitor event describing a publish/deliver event to every
 * registered WebSocket monitor without holding monitor_mutex across sends. */
static void
_mq_broadcast(csilk_mq_t* mq, const char* event, const char* topic, size_t len)
{
    if (!mq) {
        return;
    }

    size_t        inline_cap = 0;
    size_t        dispatch_cap = 0;
    csilk_ctx_t*  inline_stack[16];
    csilk_ctx_t*  dispatch_stack[16];
    csilk_ctx_t** inline_targets = inline_stack;
    csilk_ctx_t** dispatch_targets = dispatch_stack;
    size_t        inline_count = 0;
    size_t        dispatch_count = 0;

    csilk_mutex_lock(&mq->monitor_mutex);
    size_t max_n = mq->monitor_count;
    if (max_n > 16) {
        inline_targets = malloc(max_n * sizeof(csilk_ctx_t*));
        dispatch_targets = malloc(max_n * sizeof(csilk_ctx_t*));
        inline_cap = inline_targets ? max_n : 0;
        dispatch_cap = dispatch_targets ? max_n : 0;
    } else {
        inline_cap = 16;
        dispatch_cap = 16;
    }
    for (size_t i = 0; i < mq->monitor_count;) {
        csilk_ctx_t* mc = mq->monitors[i];
        if (!mc || csilk_ctx_is_closed(mc)) {
            for (size_t j = i; j + 1 < mq->monitor_count; j++) {
                mq->monitors[j] = mq->monitors[j + 1];
            }
            mq->monitor_count--;
            continue;
        }
        csilk_client_t* cl = (csilk_client_t*)_csilk_get_internal_client(mc);
        if (cl != NULL) {
            if (dispatch_count < dispatch_cap) {
                csilk_client_ref(cl);
                dispatch_targets[dispatch_count++] = mc;
            }
        } else if (inline_count < inline_cap) {
            inline_targets[inline_count++] = mc;
        }
        i++;
    }
    csilk_mutex_unlock(&mq->monitor_mutex);

    if (inline_count == 0 && dispatch_count == 0) {
        if (inline_targets != inline_stack) {
            free(inline_targets);
        }
        if (dispatch_targets != dispatch_stack) {
            free(dispatch_targets);
        }
        return;
    }

    CSILK_LOG_T("MQ: Broadcasting monitor event '%s' for topic '%s' (len: %zu) to %zu monitors",
                event,
                topic ? topic : "",
                len,
                inline_count + dispatch_count);

    csilk_json_t* root = csilk_json_object();
    char*         json = NULL;
    if (root) {
        csilk_json_add_string(root, "event", event);
        if (topic) {
            csilk_json_add_string(root, "topic", topic);
        }
        csilk_json_add_number(root, "payload_len", (double)len);
        csilk_json_add_number(root, "timestamp", (double)time(NULL));
        json = csilk_json_serialize(root, NULL);
        csilk_json_free(root);
    }

    size_t json_len = json ? strlen(json) : 0;
    if (json) {
        for (size_t i = 0; i < inline_count; i++) {
            if (!csilk_ctx_is_closed(inline_targets[i])) {
                csilk_ws_send(inline_targets[i], (const uint8_t*)json, json_len, 0x1);
            }
        }
        for (size_t i = 0; i < dispatch_count; i++) {
            csilk_ctx_t*            mc = dispatch_targets[i];
            csilk_client_t*         cl = (csilk_client_t*)_csilk_get_internal_client(mc);
            mq_ws_broadcast_task_t* t = malloc(sizeof(mq_ws_broadcast_task_t));
            char*                   jcopy = malloc(json_len + 1);
            if (!t || !jcopy) {
                free(t);
                free(jcopy);
                if (cl) {
                    csilk_client_unref(cl);
                }
                continue;
            }
            memcpy(jcopy, json, json_len + 1);
            t->client = cl;
            t->ctx = mc;
            t->json = jcopy;
            t->len = json_len;
            if (_csilk_dispatch_try(mc, _mq_broadcast_dispatch_cb, t) != 0) {
                if (cl) {
                    csilk_client_unref(cl);
                }
                free(t->json);
                free(t);
            }
        }
        free(json);
    } else {
        for (size_t i = 0; i < dispatch_count; i++) {
            csilk_client_t* cl = (csilk_client_t*)_csilk_get_internal_client(dispatch_targets[i]);
            if (cl) {
                csilk_client_unref(cl);
            }
        }
    }

    if (inline_targets != inline_stack) {
        free(inline_targets);
    }
    if (dispatch_targets != dispatch_stack) {
        free(dispatch_targets);
    }
}

/**
 * @brief Internal: Enqueue a deep-copied message and signal the event loop.
 *
 * Allocates a message node, copies the topic (strdup) and payload (malloc with
 * a trailing NUL), appends it to the queue under queue_mutex, bumps the
 * published/depth counters, broadcasts a "mq_published" monitor event, and
 * triggers the async handle.  In the io_uring backend with no running loop the
 * queue is processed synchronously inline.
 *
 * @param[in] mq      The MQ instance.
 * @param[in] topic   NUL-terminated topic name (copied).
 * @param[in] payload Opaque payload bytes (copied; may be NULL if len == 0).
 * @param[in] len     Byte length of @p payload.
 * @return 0 on success, -1 on allocation failure.
 */
CSILK_INTERNAL int
_mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
    CSILK_LOG_T("MQ: Enqueuing message on topic '%s' (len: %zu)", topic, len);
    csilk_mq_msg_t* msg = calloc(1, sizeof(csilk_mq_msg_t));
    if (!msg) {
        CSILK_LOG_E("MQ: Failed to allocate memory for message enqueued on topic '%s'", topic);
        return -1;
    }
    msg->topic = strdup(topic);
    if (!msg->topic) {
        CSILK_LOG_E("MQ: Failed to duplicate topic name '%s' for enqueued message", topic);
        free(msg);
        return -1;
    }
    if (len > 0 && payload) {
        msg->payload = malloc(len + 1);
        if (!msg->payload) {
            CSILK_LOG_E("MQ: Failed to allocate payload memory (len: %zu) for enqueued "
                        "message on topic '%s'",
                        len,
                        topic);
            free(msg->topic);
            free(msg);
            return -1;
        }
        memcpy(msg->payload, payload, len);
        ((char*)msg->payload)[len] = '\0';
        msg->len = len;
    }

    csilk_mutex_lock(&mq->queue_mutex);
    if (mq->queue_tail) {
        mq->queue_tail->next = msg;
    } else {
        mq->queue_head = msg;
    }
    mq->queue_tail = msg;

    mq->published_total++;
    mq->queue_depth++;
    csilk_mutex_unlock(&mq->queue_mutex);

    _mq_broadcast(mq, "mq_published", topic, len);
    csilk_io_async_send(&mq->async_handle);

    /* In the io_uring backend, async_send is a no-op (eventfd write).
     * If no event loop is running, process the queue synchronously. */
#ifndef CSILK_USE_URING
    (void)0; /* the async handle dispatches on the event loop automatically */
#else
    {
        csilk_mutex_lock(&mq->queue_mutex);
        int has_messages = (mq->queue_head != NULL);
        csilk_mutex_unlock(&mq->queue_mutex);
        if (has_messages) {
            /* Simulate async wake by processing the queue directly */
            csilk_io_async_t fake_async = {0};
            fake_async.data = mq;
            on_mq_async((csilk_io_async_t*)&fake_async);
        }
    }
#endif
    return 0;
}

/**
 * @brief Publish a message to a topic (durable + asynchronous).
 *
 * If WAL persistence is enabled, appends the message to the WAL first and
 * fails the publish on WAL error; then enqueues a deep copy for delivery on
 * the main event loop.  Thread-safe and non-blocking from worker threads.
 *
 * @param[in] mq      The MQ instance (must not be NULL).
 * @param[in] topic   Target topic name (must not be NULL).
 * @param[in] payload Opaque payload data (copied internally; NULL if len == 0).
 * @param[in] len     Byte length of @p payload.
 * @return 0 on success, -1 on invalid arguments, WAL failure, or enqueue failure.
 */
int
csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
    if (!mq || !topic) {
        CSILK_LOG_E(
            "MQ: Publish failed: invalid arguments (mq: %p, topic: %p)", (void*)mq, (void*)topic);
        return -1;
    }

    CSILK_LOG_D("MQ: Publishing message on topic '%s' (len: %zu)", topic, len);

    if (_mq_append_wal(mq, topic, payload, len) != 0) {
        CSILK_LOG_E("MQ: Failed to append message to WAL for topic '%s'", topic);
        return -1;
    }

    return _mq_enqueue(mq, topic, payload, len);
}

/**
 * @brief Internal: Async callback that drains and delivers queued messages.
 *
 * Invoked by the event loop when the async handle fires.  Atomically snapshots
 * and clears the in-memory queue, then for each message builds the handler
 * chain (global middleware + every topic matched via fnmatch), runs it through
 * csilk_mq_next, and frees the message.  Also broadcasts "mq_delivered" events
 * and bumps delivered_total.
 *
 * @param[in] handle The async handle whose data points to the csilk_mq_t.
 */
CSILK_INTERNAL void
on_mq_async(csilk_io_async_t* handle)
{
    csilk_mq_t* mq = (csilk_mq_t*)handle->data;

    csilk_mutex_lock(&mq->queue_mutex);
    csilk_mq_msg_t* head = mq->queue_head;
    mq->queue_head = NULL;
    mq->queue_tail = NULL;
    uint32_t count = mq->queue_depth;
    mq->queue_depth = 0;
    csilk_mutex_unlock(&mq->queue_mutex);

    CSILK_LOG_D("MQ: Async worker triggered processing %u messages from queue", count);

    while (head) {
        csilk_mq_msg_t* msg = head;
        head = head->next;

        CSILK_LOG_T(
            "MQ: Processing message from queue. Topic: '%s', Length: %zu", msg->topic, msg->len);

        _mq_broadcast(mq, "mq_delivered", msg->topic, msg->len);
        csilk_mutex_lock(&mq->queue_mutex);
        mq->delivered_total++;
        csilk_mutex_unlock(&mq->queue_mutex);

        /* Build the handler chain under the topics lock so the count, the
         * allocation, and the copy form a single atomic unit.  A concurrent
         * csilk_mq_use() on a worker thread cannot grow handler_count between
         * the count and the memcpy (TOCTOU). */
        csilk_mutex_lock(&mq->topics_mutex);

        size_t total_handlers = mq->global_mw_count;
        for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
            if (fnmatch(t->name, msg->topic, 0) == 0) {
                total_handlers += t->handler_count;
            }
        }

        if (total_handlers > 0) {
            csilk_mq_handler_t* chain = malloc(total_handlers * sizeof(csilk_mq_handler_t));
            if (chain) {
                size_t idx = 0;

                if (mq->global_mw_count > 0) {
                    memcpy(chain,
                           mq->global_middlewares,
                           mq->global_mw_count * sizeof(csilk_mq_handler_t));
                    idx += mq->global_mw_count;
                }

                for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
                    if (fnmatch(t->name, msg->topic, 0) == 0 && t->handler_count > 0) {
                        memcpy(chain + idx,
                               t->handlers,
                               t->handler_count * sizeof(csilk_mq_handler_t));
                        idx += t->handler_count;
                    }
                }
                csilk_mutex_unlock(&mq->topics_mutex);

                CSILK_LOG_T("MQ: Executing handler chain with %zu total handlers "
                            "for topic '%s'",
                            total_handlers,
                            msg->topic);
                csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1, 0};
                csilk_mq_next(&ctx);
                free(chain);
            } else {
                csilk_mutex_unlock(&mq->topics_mutex);
                CSILK_LOG_E("MQ: Failed to allocate memory for handler chain "
                            "(total_handlers: %zu) for topic '%s'",
                            total_handlers,
                            msg->topic);
            }
        } else {
            csilk_mutex_unlock(&mq->topics_mutex);
            CSILK_LOG_D("MQ: No handlers registered for topic '%s'", msg->topic);
        }

        free(msg->topic);
        free(msg->payload);
        free(msg);
    }
}
