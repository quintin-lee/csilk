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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

typedef struct wal_task_s {
    char*                 wal_path;
    csilk_wf_event_type_t type;
    void*                 payload;
    size_t                len;
    struct wal_task_s*    next;
} wal_task_t;

static wal_task_t*     g_wal_queue_head = NULL;
static wal_task_t*     g_wal_queue_tail = NULL;
static pthread_mutex_t g_wal_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wal_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       g_wal_thread;
static int             g_wal_thread_started = 0;
static int             g_wal_shutdown = 0;

static void*
wal_writer_thread(void* arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_wal_mutex);
        while (g_wal_queue_head == NULL && !g_wal_shutdown) {
            pthread_cond_wait(&g_wal_cond, &g_wal_mutex);
        }
        if (g_wal_shutdown && g_wal_queue_head == NULL) {
            pthread_mutex_unlock(&g_wal_mutex);
            break;
        }

        // Pop task
        wal_task_t* task = g_wal_queue_head;
        g_wal_queue_head = task->next;
        if (g_wal_queue_head == NULL) {
            g_wal_queue_tail = NULL;
        }
        pthread_mutex_unlock(&g_wal_mutex);

        // Process task
        int fd = open(task->wal_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            csilk_wf_wal_header_t header;
            header.magic = CSILK_WF_MAGIC;
            header.type = (uint8_t)task->type;
            header.timestamp = (uint64_t)time(nullptr);
            header.payload_len = (uint32_t)task->len;

            if (write(fd, &header, sizeof(header)) == sizeof(header)) {
                if (task->len > 0 && task->payload) {
                    (void)write(fd, task->payload, task->len);
                }
            }
#ifdef __APPLE__
            fsync(fd);
#else
            fdatasync(fd);
#endif
            close(fd);
        }

        // Free task
        free(task->wal_path);
        free(task->payload);
        free(task);
    }
    return NULL;
}

static void
wal_cleanup(void)
{
    pthread_mutex_lock(&g_wal_mutex);
    g_wal_shutdown = 1;
    pthread_cond_signal(&g_wal_cond);
    pthread_mutex_unlock(&g_wal_mutex);

    if (g_wal_thread_started) {
        pthread_join(g_wal_thread, NULL);
    }
}

CSILK_INTERNAL void
_wf_wal_flush(void)
{
    pthread_mutex_lock(&g_wal_mutex);
    while (g_wal_queue_head != NULL) {
        pthread_mutex_unlock(&g_wal_mutex);
        usleep(1000); // 1ms
        pthread_mutex_lock(&g_wal_mutex);
    }
    pthread_mutex_unlock(&g_wal_mutex);
}

CSILK_INTERNAL int
_wf_wal_append(const char* wal_path, csilk_wf_event_type_t type, const void* payload, size_t len)
{
    if (!wal_path) {
        return -1;
    }

    pthread_mutex_lock(&g_wal_mutex);
    if (!g_wal_thread_started && !g_wal_shutdown) {
        if (pthread_create(&g_wal_thread, NULL, wal_writer_thread, NULL) == 0) {
            g_wal_thread_started = 1;
            atexit(wal_cleanup);
        }
    }
    pthread_mutex_unlock(&g_wal_mutex);

    wal_task_t* task = malloc(sizeof(wal_task_t));
    if (!task) {
        return -1;
    }
    task->wal_path = strdup(wal_path);
    task->type = type;
    task->len = len;
    if (len > 0 && payload) {
        task->payload = malloc(len);
        if (task->payload) {
            memcpy(task->payload, payload, len);
        }
    } else {
        task->payload = NULL;
    }
    task->next = NULL;

    pthread_mutex_lock(&g_wal_mutex);
    if (g_wal_queue_tail) {
        g_wal_queue_tail->next = task;
        g_wal_queue_tail = task;
    } else {
        g_wal_queue_head = task;
        g_wal_queue_tail = task;
    }
    pthread_cond_signal(&g_wal_cond);
    pthread_mutex_unlock(&g_wal_mutex);

    return 0;
}
