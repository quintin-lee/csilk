/**
 * @file logger.c
 * @brief High-performance asynchronous lock-free structured logger.
 *
 * Architecture:
 *   Producers (Workers):
 *     1. Zero-overhead atomic filter check (CSILK_LOG_IS_ENABLED)
 *     2. Lock-free node acquisition from pre-allocated slab (Treiber stack)
 *     3. In-place TLS/stack formatting (Text / JSON with request-ID)
 *     4. Intrusive wait-free MPSC enqueue (csilk_lfqueue_t)
 *     5. Configurable overflow policies: DROP, BLOCK, FALLBACK
 *
 *   Consumer (Background Logger Thread):
 *     1. Batch dequeue from lock-free MPSC queue
 *     2. Atomic file size accounting and single-backup file rotation (.1)
 *     3. Single-threaded non-blocking disk I/O (fwrite, periodic fflush)
 *     4. Return nodes to lock-free pool
 *     5. Crash-safe shutdown flushing
 *
 * @copyright MIT License
 */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "csilk/core/server/server.h"
#include "../primitives/lfqueue.h"

/* Exported atomic level and init flags for zero-overhead inline macro checks */
_Atomic(int) g_csilk_log_level_val = CSILK_LOG_INFO;
_Atomic(int) g_csilk_log_is_init = 0;

#define CSILK_LOG_NODE_BUF_SIZE 2048
#define CSILK_LOG_DEFAULT_QUEUE_CAPACITY 8192

/** @brief Pre-allocated node in the lock-free logging ring/slab. */
typedef struct csilk_log_node_s {
    csilk_lfq_node_t lfq_node;           /**< Intrusive node for MPSC queue (must be first). */
    _Atomic(struct csilk_log_node_s*)
           next_free;                    /**< Intrusive link for lock-free node pool stack. */
    size_t len;                          /**< Number of bytes in buf. */
    char   buf[CSILK_LOG_NODE_BUF_SIZE]; /**< Formatted line. */
} csilk_log_node_t;

/** @brief Internal asynchronous logger singleton. */
typedef struct {
    csilk_log_config_t config;       /**< Logger configuration. */
    FILE*              fp;           /**< Output file pointer. */
    size_t             current_size; /**< Current log file size (updated by consumer). */
    int                initialized;  /**< Local initialization flag. */

    /* Lock-free MPSC queue & Node Pool */
    csilk_lfqueue_t            queue;
    _Atomic(csilk_log_node_t*) free_stack;
    csilk_log_node_t*          node_slab;
    size_t                     slab_capacity;

    /* Thread management & synchronization */
    csilk_thread_t    consumer_tid;
    _Atomic(int)      running;
    _Atomic(int)      consumer_sleeping;
    _Atomic(uint64_t) dropped_count;
    _Atomic(uint64_t) queued_seq;
    _Atomic(uint64_t) flushed_seq;

    csilk_mutex_t wake_mutex;
    csilk_cond_t  wake_cond;
    csilk_mutex_t fallback_mutex; /**< Mutex for fallback sync writing / direct stderr. */
} csilk_async_logger_t;

static csilk_async_logger_t g_logger = {0};

static _Thread_local char tl_request_id[CSILK_UUID_BUF_SIZE];

/** @brief Return this thread's request-ID buffer (thread-local). */
static inline char*
get_tl_request_id(void)
{
    return tl_request_id;
}

static const char* level_names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
static const char* level_colors[] = {
    "\x1b[35m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[41;1m"};

/** @brief Cached formatted timestamp, refreshed once per second. */
static _Thread_local struct {
    time_t last_sec;
    char   text[20]; /**< "YYYY-MM-DD HH:MM:SS" (19 chars + NUL) */
} tls_time_cache = {0, {0}};

/* --- Lock-Free Node Pool (Treiber Stack) --- */

static inline csilk_log_node_t*
log_pool_pop(void)
{
    csilk_log_node_t* head = atomic_load_explicit(&g_logger.free_stack, memory_order_acquire);
    while (head) {
        csilk_log_node_t* next = atomic_load_explicit(&head->next_free, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(
                &g_logger.free_stack, &head, next, memory_order_release, memory_order_acquire)) {
            return head;
        }
    }
    return NULL;
}

static inline void
log_pool_push(csilk_log_node_t* node)
{
    csilk_log_node_t* head = atomic_load_explicit(&g_logger.free_stack, memory_order_relaxed);
    do {
        atomic_store_explicit(&node->next_free, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        &g_logger.free_stack, &head, node, memory_order_release, memory_order_relaxed));
}

/* --- File Rotation (Run exclusively by consumer thread) --- */

static void
rotate_log_files(void)
{
    if (!g_logger.config.file_path || !g_logger.fp) {
        return;
    }
    if (g_logger.fp != stdout && g_logger.fp != stderr) {
        fclose(g_logger.fp);
    }
    char old[512];
    snprintf(old, sizeof(old), "%s.1", g_logger.config.file_path);
    rename(g_logger.config.file_path, old);
    g_logger.fp = fopen(g_logger.config.file_path, "a");
    g_logger.current_size = 0;
}

/* --- Formatting Helpers --- */

static inline void
update_time_cache(time_t* out_now)
{
    time_t now = time(NULL);
    if (out_now) {
        *out_now = now;
    }
    if (now != tls_time_cache.last_sec) {
        tls_time_cache.last_sec = now;
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(tls_time_cache.text, sizeof(tls_time_cache.text), "%Y-%m-%d %H:%M:%S", &tm);
    }
}

static size_t
format_text_line(char*             dest,
                 size_t            cap,
                 csilk_log_level_t lv,
                 const char*       file,
                 int               line,
                 const char*       func,
                 const char*       msg,
                 int               msg_len)
{
    const char* fn = strrchr(file, '/');
    fn = fn ? fn + 1 : file;

    update_time_cache(NULL);

    int n = 0;
    if (g_logger.config.use_colors) {
        n = snprintf(dest,
                     cap,
                     "%s %s%s\x1b[0m [%s:%d] %s(): ",
                     tls_time_cache.text,
                     level_colors[lv],
                     level_names[lv],
                     fn,
                     line,
                     func);
    } else {
        n = snprintf(dest,
                     cap,
                     "%s %s [%s:%d] %s(): ",
                     tls_time_cache.text,
                     level_names[lv],
                     fn,
                     line,
                     func);
    }

    if (n < 0) {
        n = 0;
        dest[0] = '\0';
    } else if ((size_t)n >= cap) {
        n = (int)cap - 1;
    }

    char* tl_req = get_tl_request_id();
    if (tl_req[0] != '\0' && (size_t)n < cap) {
        int req_n = snprintf(dest + n, cap - (size_t)n, "<%s> ", tl_req);
        if (req_n > 0) {
            n += req_n;
            if ((size_t)n >= cap) {
                n = (int)cap - 1;
            }
        }
    }

    if (msg && msg_len > 0 && (size_t)n < cap) {
        size_t remaining = cap - (size_t)n;
        size_t to_copy = (size_t)msg_len < remaining ? (size_t)msg_len : remaining;
        memcpy(dest + n, msg, to_copy);
        n += (int)to_copy;
    }

    /* Add newline */
    if (n >= 0 && (size_t)n < cap - 1) {
        dest[n++] = '\n';
        dest[n] = '\0';
    } else if (cap > 1) {
        dest[cap - 2] = '\n';
        dest[cap - 1] = '\0';
        n = (int)cap - 1;
    }

    return (size_t)n;
}

static size_t
format_json_line(char*             dest,
                 size_t            cap,
                 csilk_log_level_t lv,
                 const char*       file,
                 int               line,
                 const char*       func,
                 csilk_json_t*     extra,
                 const char*       msg,
                 int               msg_len)
{
    const char* fn = strrchr(file, '/');
    fn = fn ? fn + 1 : file;

    time_t now = 0;
    update_time_cache(&now);

    csilk_json_t* root = csilk_json_object();
    if (!root) {
        if (extra) {
            csilk_json_free(extra);
        }
        return 0;
    }

    csilk_json_add_number(root, "time_epoch", (double)(int64_t)now);
    csilk_json_add_string(root, "level", level_names[lv]);

    char* tl_req = get_tl_request_id();
    csilk_json_add_string(root, "request_id", tl_req);

    csilk_json_add_string(root, "file", fn);
    csilk_json_add_number(root, "line", line);
    csilk_json_add_string(root, "func", func);

    if (msg && msg_len > 0) {
        size_t cp = (size_t)msg_len < 1023 ? (size_t)msg_len : 1023;
        char   tmp[1024];
        memcpy(tmp, msg, cp);
        tmp[cp] = '\0';
        csilk_json_add_string(root, "msg", tmp);
    } else {
        csilk_json_add_string(root, "msg", "");
    }

    if (extra) {
        size_t count = csilk_json_object_size(extra);
        for (size_t i = 0; i < count; i++) {
            const char*         key = csilk_json_object_key(extra, i);
            const csilk_json_t* val = csilk_json_object_val(extra, i);
            if (key && val) {
                csilk_json_t* dupe = csilk_json_copy(val);
                if (dupe) {
                    csilk_json_add_object(root, key, dupe);
                }
            }
        }
        csilk_json_free(extra);
    }

    char*  serialized = csilk_json_serialize(root, NULL);
    size_t len = 0;
    if (serialized) {
        size_t slen = strlen(serialized);
        if (slen + 2 <= cap) {
            memcpy(dest, serialized, slen);
            dest[slen] = '\n';
            dest[slen + 1] = '\0';
            len = slen + 1;
        } else if (cap > 2) {
            memcpy(dest, serialized, cap - 2);
            dest[cap - 2] = '\n';
            dest[cap - 1] = '\0';
            len = cap - 1;
        }
        free(serialized);
    }
    csilk_json_free(root);
    return len;
}

/* --- Consumer Background Thread --- */

static void
logger_consumer_loop(void* arg)
{
    (void)arg;

    while (atomic_load_explicit(&g_logger.running, memory_order_acquire) ||
           atomic_load_explicit(&g_logger.queued_seq, memory_order_acquire) >
               atomic_load_explicit(&g_logger.flushed_seq, memory_order_acquire)) {

        int               processed = 0;
        csilk_lfq_node_t* raw_node = NULL;

        while ((raw_node = csilk_lfq_dequeue(&g_logger.queue)) != NULL) {
            csilk_log_node_t* node = (csilk_log_node_t*)raw_node;

            /* Check file rotation */
            if (g_logger.config.max_file_size > 0 &&
                g_logger.current_size + node->len >= g_logger.config.max_file_size) {
                rotate_log_files();
            }

            if (g_logger.fp && node->len > 0) {
                fwrite(node->buf, 1, node->len, g_logger.fp);
                g_logger.current_size += node->len;
            }

            log_pool_push(node);
            processed++;
            atomic_fetch_add_explicit(&g_logger.flushed_seq, 1, memory_order_release);
        }

        if (processed > 0 && g_logger.fp) {
            fflush(g_logger.fp);
        }

        if (!atomic_load_explicit(&g_logger.running, memory_order_acquire) &&
            atomic_load_explicit(&g_logger.queued_seq, memory_order_acquire) ==
                atomic_load_explicit(&g_logger.flushed_seq, memory_order_acquire)) {
            break;
        }

        if (processed == 0) {
            atomic_store_explicit(&g_logger.consumer_sleeping, 1, memory_order_release);
            csilk_mutex_lock(&g_logger.wake_mutex);
            csilk_cond_timedwait(&g_logger.wake_cond, &g_logger.wake_mutex, 5000000ULL /* 5 ms */);
            csilk_mutex_unlock(&g_logger.wake_mutex);
            atomic_store_explicit(&g_logger.consumer_sleeping, 0, memory_order_release);
        }
    }

    if (g_logger.fp) {
        fflush(g_logger.fp);
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

int
csilk_log_init(csilk_log_config_t config)
{
    if (g_logger.initialized) {
        csilk_log_close();
    }

    g_logger.config = config;
    if (config.file_path) {
        g_logger.fp = fopen(config.file_path, "a");
        if (!g_logger.fp) {
            return -1;
        }
        struct stat st;
        if (stat(config.file_path, &st) == 0) {
            g_logger.current_size = (size_t)st.st_size;
        } else {
            g_logger.current_size = 0;
        }
    } else {
        g_logger.fp = stdout;
        g_logger.config.max_file_size = 0;
        g_logger.current_size = 0;
    }

    if (g_logger.config.use_colors == -1) {
        g_logger.config.use_colors = isatty(fileno(g_logger.fp));
    }

    size_t cap =
        config.queue_capacity > 0 ? config.queue_capacity : CSILK_LOG_DEFAULT_QUEUE_CAPACITY;
    g_logger.slab_capacity = cap;
    g_logger.node_slab = calloc(cap, sizeof(csilk_log_node_t));
    if (!g_logger.node_slab) {
        if (config.file_path && g_logger.fp) {
            fclose(g_logger.fp);
        }
        return -1;
    }

    /* Initialize MPSC Queue & Node Pool */
    csilk_lfq_init(&g_logger.queue);
    atomic_init(&g_logger.free_stack, NULL);
    for (size_t i = 0; i < cap; i++) {
        log_pool_push(&g_logger.node_slab[i]);
    }

    atomic_init(&g_logger.dropped_count, 0);
    atomic_init(&g_logger.queued_seq, 0);
    atomic_init(&g_logger.flushed_seq, 0);
    atomic_init(&g_logger.running, 1);
    atomic_init(&g_logger.consumer_sleeping, 0);

    csilk_mutex_init(&g_logger.wake_mutex);
    csilk_cond_init(&g_logger.wake_cond);
    csilk_mutex_init(&g_logger.fallback_mutex);

    /* Start background consumer thread */
    if (csilk_thread_create(&g_logger.consumer_tid, logger_consumer_loop, NULL) != 0) {
        free(g_logger.node_slab);
        g_logger.node_slab = NULL;
        if (config.file_path && g_logger.fp) {
            fclose(g_logger.fp);
        }
        return -1;
    }

    atomic_store_explicit(&g_csilk_log_level_val, (int)config.level, memory_order_release);
    atomic_store_explicit(&g_csilk_log_is_init, 1, memory_order_release);
    g_logger.initialized = 1;

    return 0;
}

void
csilk_log_flush(void)
{
    if (!g_logger.initialized) {
        return;
    }

    uint64_t target = atomic_load_explicit(&g_logger.queued_seq, memory_order_acquire);
    csilk_cond_signal(&g_logger.wake_cond);

    while (atomic_load_explicit(&g_logger.flushed_seq, memory_order_acquire) < target) {
        csilk_thread_yield();
    }

    csilk_mutex_lock(&g_logger.fallback_mutex);
    if (g_logger.fp) {
        fflush(g_logger.fp);
    }
    csilk_mutex_unlock(&g_logger.fallback_mutex);
}

void
csilk_log_close(void)
{
    if (!g_logger.initialized) {
        return;
    }

    atomic_store_explicit(&g_csilk_log_is_init, 0, memory_order_release);
    atomic_store_explicit(&g_logger.running, 0, memory_order_release);

    csilk_cond_signal(&g_logger.wake_cond);
    csilk_thread_join(&g_logger.consumer_tid);

    csilk_mutex_lock(&g_logger.fallback_mutex);
    if (g_logger.fp && g_logger.fp != stdout && g_logger.fp != stderr) {
        fclose(g_logger.fp);
    }
    g_logger.fp = NULL;
    csilk_mutex_unlock(&g_logger.fallback_mutex);

    if (g_logger.node_slab) {
        free(g_logger.node_slab);
        g_logger.node_slab = NULL;
    }

    csilk_mutex_destroy(&g_logger.wake_mutex);
    csilk_cond_destroy(&g_logger.wake_cond);
    csilk_mutex_destroy(&g_logger.fallback_mutex);

    g_logger.initialized = 0;
}

/* --- Internal Log Emission --- */

static void
emit_log_payload(csilk_log_level_t lv,
                 const char*       file,
                 int               line,
                 const char*       func,
                 csilk_json_t*     extra,
                 const char*       msg,
                 int               msg_len)
{
    if (!g_logger.initialized || !atomic_load_explicit(&g_logger.running, memory_order_acquire)) {
        if (extra) {
            csilk_json_free(extra);
        }
        return;
    }

    csilk_log_node_t* node = log_pool_pop();
    if (!node) {
        if (g_logger.config.overflow_strategy == CSILK_LOG_OVERFLOW_BLOCK) {
            /* Block/spin waiting for a node */
            while (!node && atomic_load_explicit(&g_logger.running, memory_order_acquire)) {
                if (atomic_load_explicit(&g_logger.consumer_sleeping, memory_order_relaxed)) {
                    atomic_store_explicit(&g_logger.consumer_sleeping, 0, memory_order_relaxed);
                    csilk_cond_signal(&g_logger.wake_cond);
                }
                csilk_thread_yield();
                node = log_pool_pop();
            }
        }

        if (!node) {
            if (g_logger.config.overflow_strategy == CSILK_LOG_OVERFLOW_FALLBACK) {
                /* Synchronous fallback */
                char   fallback_buf[CSILK_LOG_NODE_BUF_SIZE];
                size_t flen = g_logger.config.json_format ? format_json_line(fallback_buf,
                                                                             sizeof(fallback_buf),
                                                                             lv,
                                                                             file,
                                                                             line,
                                                                             func,
                                                                             extra,
                                                                             msg,
                                                                             msg_len)
                                                          : format_text_line(fallback_buf,
                                                                             sizeof(fallback_buf),
                                                                             lv,
                                                                             file,
                                                                             line,
                                                                             func,
                                                                             msg,
                                                                             msg_len);

                csilk_mutex_lock(&g_logger.fallback_mutex);
                FILE* out = g_logger.fp ? g_logger.fp : stderr;
                fwrite(fallback_buf, 1, flen, out);
                fflush(out);
                csilk_mutex_unlock(&g_logger.fallback_mutex);
                return;
            }

            /* DROP strategy */
            atomic_fetch_add_explicit(&g_logger.dropped_count, 1, memory_order_relaxed);
            if (extra) {
                csilk_json_free(extra);
            }
            return;
        }
    }

    /* Format directly into acquired node */
    if (g_logger.config.json_format) {
        node->len = format_json_line(
            node->buf, sizeof(node->buf), lv, file, line, func, extra, msg, msg_len);
    } else {
        if (extra) {
            csilk_json_free(extra);
        }
        node->len =
            format_text_line(node->buf, sizeof(node->buf), lv, file, line, func, msg, msg_len);
    }

    csilk_lfq_enqueue(&g_logger.queue, &node->lfq_node);
    atomic_fetch_add_explicit(&g_logger.queued_seq, 1, memory_order_release);
    if (atomic_load_explicit(&g_logger.consumer_sleeping, memory_order_relaxed)) {
        atomic_store_explicit(&g_logger.consumer_sleeping, 0, memory_order_relaxed);
        csilk_cond_signal(&g_logger.wake_cond);
    }
}

CSILK_INTERNAL void
_csilk_log_internal(
    csilk_log_level_t lv, const char* file, int line, const char* func, const char* fmt, ...)
{
    if (!csilk_log_is_enabled(lv)) {
        return;
    }

    char    msg_buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args); // NOLINT
    va_end(args);
    if (len < 0) {
        len = 0;
    }
    if (len >= (int)sizeof(msg_buf)) {
        len = (int)sizeof(msg_buf) - 1;
    }

    emit_log_payload(lv, file, line, func, NULL, msg_buf, len);
}

CSILK_INTERNAL void
_csilk_log_structured(csilk_log_level_t lv,
                      const char*       file,
                      int               line,
                      const char*       func,
                      csilk_json_t*     extra,
                      const char*       fmt,
                      ...)
{
    if (!csilk_log_is_enabled(lv)) {
        if (extra) {
            csilk_json_free(extra);
        }
        return;
    }

    char    msg_buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args); // NOLINT
    va_end(args);
    if (len < 0) {
        len = 0;
    }
    if (len >= (int)sizeof(msg_buf)) {
        len = (int)sizeof(msg_buf) - 1;
    }

    emit_log_payload(lv, file, line, func, extra, msg_buf, len);
}

int
csilk_log_is_json(void)
{
    return g_logger.initialized && g_logger.config.json_format;
}

void
csilk_log_set_request_id(const char* request_id)
{
    char* tl_req = get_tl_request_id();
    if (request_id) {
        snprintf(tl_req, CSILK_UUID_BUF_SIZE, "%s", request_id);
    } else {
        tl_req[0] = '\0';
    }
}

csilk_json_t*
csilk_log_make_kv(const char* key, ...)
{
    csilk_json_t* obj = csilk_json_object();
    if (!obj) {
        return NULL;
    }
    if (!key) {
        return obj;
    }
    const char* k = key;
    va_list     args;
    va_start(args, key);
    while (k) {
        const char* v = va_arg(args, const char*); // NOLINT
        if (v) {
            csilk_json_add_string(obj, k, v);
        }
        k = va_arg(args, const char*);
    }
    va_end(args);
    return obj;
}
