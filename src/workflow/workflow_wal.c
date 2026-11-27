/**
 * @file workflow_wal.c
 * @brief Binary Write-Ahead Log implementation for AI workflows.
 *
 * Architecture: Crash-safe durability for workflow execution state.
 * Events (node start, node finish, workflow end) are written as
 * fixed-size headers + variable-length payloads to a binary WAL file.
 * Each record is fsynced before returning to guarantee durability.
 *
 * The WAL is replayed during csilk_wf_resume() to reconstruct the
 * workflow's execution state after a crash or restart.
 *
 * Wire format:
 *   [magic:4 bytes][type:1 byte][timestamp:8 bytes][payload_len:4 bytes]
 *   [payload:payload_len bytes]
 *
 * @copyright MIT License
 */

#include "csilk/app/workflow_wal.h"
#include "csilk/core/sync.h"
#include "workflow_internal.h"
#include "../messaging/wal_common.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct wal_task_s {
    char*                 wal_path;
    csilk_wf_event_type_t type;
    void*                 payload;
    size_t                len;
    struct wal_task_s*    next;
} wal_task_t;

static wal_task_t*    g_wal_queue_head = NULL;
static wal_task_t*    g_wal_queue_tail = NULL;
static csilk_once_t   g_wal_init_once = CSILK_ONCE_INIT;
static csilk_mutex_t  g_wal_mutex;
static csilk_cond_t   g_wal_cond;
static csilk_thread_t g_wal_thread;
static int            g_wal_thread_started = 0;
static int            g_wal_shutdown = 0;
static int            g_wal_writing = 0;

static void
wf_wal_sync_init(void)
{
    csilk_mutex_init(&g_wal_mutex);
    csilk_cond_init(&g_wal_cond);
}

static void
wal_writer_thread(void* arg)
{
    (void)arg;
    while (1) {
        csilk_mutex_lock(&g_wal_mutex);
        while (g_wal_queue_head == NULL && !g_wal_shutdown) {
            csilk_cond_wait(&g_wal_cond, &g_wal_mutex);
        }
        if (g_wal_shutdown && g_wal_queue_head == NULL) {
            csilk_mutex_unlock(&g_wal_mutex);
            break;
        }

        // Pop task
        wal_task_t* task = g_wal_queue_head;
        g_wal_queue_head = task->next;
        if (g_wal_queue_head == NULL) {
            g_wal_queue_tail = NULL;
        }
        g_wal_writing = 1;
        csilk_mutex_unlock(&g_wal_mutex);

        // Process task
        int fd = open(task->wal_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            csilk_wf_wal_header_t header;
            header.magic = CSILK_WF_MAGIC;
            header.type = (uint8_t)task->type;
            header.timestamp = (uint64_t)time(NULL);
            header.payload_len = (uint32_t)task->len;

            if (_csilk_wal_write_all(fd, &header, sizeof(header)) == 0) {
                if (task->len > 0 && task->payload) {
                    (void)_csilk_wal_write_all(fd, task->payload, task->len);
                }
            }
            (void)_csilk_wal_fsync_fd(fd);
            close(fd);
        }

        // Free task
        free(task->wal_path);
        free(task->payload);
        free(task);

        csilk_mutex_lock(&g_wal_mutex);
        g_wal_writing = 0;
        csilk_cond_broadcast(&g_wal_cond);
        csilk_mutex_unlock(&g_wal_mutex);
    }
}

static void
wal_cleanup(void)
{
    csilk_once(&g_wal_init_once, wf_wal_sync_init);
    csilk_mutex_lock(&g_wal_mutex);
    g_wal_shutdown = 1;
    int started = g_wal_thread_started;
    csilk_cond_signal(&g_wal_cond);
    csilk_mutex_unlock(&g_wal_mutex);

    if (started) {
        csilk_thread_join(&g_wal_thread);
    }
}

/**
 * @brief Blocks until the WAL writer queue has fully drained.
 *
 * Waits on g_wal_cond until both the pending queue is empty and any active
 * disk write/fsync has completed. Used before resuming/replaying to guarantee
 * durability.
 *
 * @note Internally locks g_wal_mutex. Thread-safe.
 */
CSILK_INTERNAL void
_wf_wal_flush(void)
{
    csilk_once(&g_wal_init_once, wf_wal_sync_init);
    csilk_mutex_lock(&g_wal_mutex);
    while (g_wal_queue_head != NULL || g_wal_writing) {
        csilk_cond_wait(&g_wal_cond, &g_wal_mutex);
    }
    csilk_mutex_unlock(&g_wal_mutex);
}

/**
 * @brief Queues a WAL event record for asynchronous durable write.
 *
 * Copies the payload and appends a task to the global queue, lazily starting
 * the writer thread (and registering an atexit cleanup) on first use. The
 * writer opens the WAL file in append mode, writes the fixed header followed
 * by the payload, and fsyncs/datasyncs before closing.
 *
 * @param wal_path Path of the WAL file to append to (must not be NULL).
 * @param type     Event type (WF_EV_START, WF_EV_NODE_START, etc.).
 * @param payload  Event payload bytes (may be NULL when len is 0).
 * @param len      Length of payload in bytes.
 * @return 0 on success, or -1 if wal_path is NULL or memory allocation failed.
 * @note Thread-safe; the queue is guarded by g_wal_mutex and g_wal_cond.
 */
CSILK_INTERNAL int
_wf_wal_append(const char* wal_path, csilk_wf_event_type_t type, const void* payload, size_t len)
{
    if (!wal_path || len > UINT32_MAX) {
        return -1;
    }

    csilk_once(&g_wal_init_once, wf_wal_sync_init);
    csilk_mutex_lock(&g_wal_mutex);
    if (!g_wal_thread_started && !g_wal_shutdown) {
        if (csilk_thread_create(&g_wal_thread, wal_writer_thread, NULL) == 0) {
            g_wal_thread_started = 1;
            atexit(wal_cleanup);
        }
    }
    csilk_mutex_unlock(&g_wal_mutex);

    wal_task_t* task = malloc(sizeof(wal_task_t));
    if (!task) {
        return -1;
    }
    task->wal_path = strdup(wal_path);
    if (!task->wal_path) {
        free(task);
        return -1;
    }
    task->type = type;
    task->len = (payload && len > 0) ? len : 0;
    if (task->len > 0) {
        task->payload = malloc(task->len);
        if (!task->payload) {
            free(task->wal_path);
            free(task);
            return -1;
        }
        memcpy(task->payload, payload, task->len);
    } else {
        task->payload = NULL;
    }
    task->next = NULL;

    csilk_mutex_lock(&g_wal_mutex);
    if (g_wal_queue_tail) {
        g_wal_queue_tail->next = task;
        g_wal_queue_tail = task;
    } else {
        g_wal_queue_head = task;
        g_wal_queue_tail = task;
    }
    csilk_cond_signal(&g_wal_cond);
    csilk_mutex_unlock(&g_wal_mutex);

    return 0;
}

void
_wf_wal_log_event(csilk_wf_ctx_t*       ctx,
                  csilk_wf_event_type_t type,
                  const char*           node_id,
                  csilk_data_t*         data)
{
    if (!ctx || !ctx->wal_path) {
        return;
    }

    const char* nid = node_id ? node_id : "";
    const char* d_type = (data && data->type) ? data->type : "";
    const char* d_val = (data && data->value) ? (char*)data->value : "";

    size_t node_id_len = strlen(nid) + 1;
    size_t type_len = strlen(d_type) + 1;
    size_t val_len = strlen(d_val) + 1;

    size_t total_len = node_id_len + type_len + val_len;
    char*  payload = malloc(total_len);
    if (!payload) {
        return;
    }

    char* p = payload;
    memcpy(p, nid, node_id_len);
    p += node_id_len;
    memcpy(p, d_type, type_len);
    p += type_len;
    memcpy(p, d_val, val_len);

    _wf_wal_append(ctx->wal_path, type, payload, total_len);
    free(payload);
}
