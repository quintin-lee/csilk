#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/core/internal.h"
#include "mq_internal.h"
#include "csilk/core/sync.h"
#include "csilk/messaging/mq.h"
#include "mq_internal.h"

extern void on_mq_async(csilk_io_async_t* handle);

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

char*
csilk_mq_stats_to_json(const csilk_mq_stats_t* stats)
{
    if (!stats) {
        return nullptr;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "published_total", (double)stats->published_total);
    cJSON_AddNumberToObject(root, "delivered_total", (double)stats->delivered_total);
    cJSON_AddNumberToObject(root, "failed_total", (double)stats->failed_total);
    cJSON_AddNumberToObject(root, "queue_depth", (double)stats->queue_depth);
    cJSON_AddNumberToObject(root, "topic_count", (double)stats->topic_count);
    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

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

CSILK_INTERNAL csilk_mq_t*
_csilk_mq_new(csilk_io_loop_t* loop)
{
    csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
    if (!mq) {
        CSILK_LOG_E("MQ: Failed to allocate memory for message queue");
        return nullptr;
    }

    csilk_mutex_init(&mq->queue_mutex);
    csilk_mutex_init(&mq->monitor_mutex);
    mq->loop = loop;

    csilk_io_async_init(loop, &mq->async_handle, on_mq_async);
    mq->async_handle.data = mq;

    mq->wal_fd = -1;
    mq->wal_path = nullptr;
    csilk_mutex_init(&mq->wal_mutex);

    CSILK_LOG_I("MQ: Message queue initialized successfully");
    return mq;
}

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
        csilk_io_fs_close(handle->loop, &close_req, mq->wal_fd, nullptr);
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

    CSILK_LOG_I("MQ: Message queue closed and resource cleanup complete");
    free(mq);
}

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
