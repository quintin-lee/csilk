/**
 * @file logger.c
 * @brief Thread-safe logging with JSON structured output, file rotation, ANSI colors.
 *
 * Uses the csilk reflection engine for JSON serialization of log entries.
 * @copyright MIT License
 * @version 0.2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uv.h>
#include "csilk.h"
#include "csilk_reflect.h"

/* ---- reflectable log-entry struct ---- */

/** @brief Structured log entry for JSON serialization via reflection. */
typedef struct csilk_log_entry_s {
    int64_t  time_epoch;   /**< unix timestamp */
    char     level[8];     /**< TRACE/DEBUG/INFO/WARN/ERROR/FATAL */
    char     file[64];     /**< source filename */
    int32_t  line;         /**< source line number */
    char     func[64];     /**< function name */
    char     msg[1024];    /**< log message */
} csilk_log_entry_t;

#define LOG_ENTRY_MAP(X) \
    X(csilk_log_entry_t, time_epoch, CSILK_TYPE_INT64,  sizeof(int64_t),  0, false, NULL) \
    X(csilk_log_entry_t, level,      CSILK_TYPE_STRING, 8,               0, false, NULL) \
    X(csilk_log_entry_t, file,       CSILK_TYPE_STRING, 64,              0, false, NULL) \
    X(csilk_log_entry_t, line,       CSILK_TYPE_INT32,  sizeof(int32_t), 0, false, NULL) \
    X(csilk_log_entry_t, func,       CSILK_TYPE_STRING, 64,              0, false, NULL) \
    X(csilk_log_entry_t, msg,        CSILK_TYPE_STRING, 1024,            0, false, NULL)

CSILK_REGISTER_REFLECT(csilk_log_entry_t, LOG_ENTRY_MAP)

/* ---- internal logger state ---- */

/** @brief Internal logger state (singleton). */
typedef struct {
    csilk_log_config_t config; /**< Logger configuration. */
    FILE*              fp;     /**< Output file pointer. */
    size_t             current_size; /**< Current log file size. */
    uv_mutex_t         mutex;  /**< Mutex for thread-safe logging. */
    int                initialized; /**< Whether logger is initialized. */
} csilk_logger_t;

static csilk_logger_t g_logger = { {0}, NULL, 0, {0}, 0 };

static const char* level_names[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};
static const char* level_colors[] = {
    "\x1b[35m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[41;1m"
};

/* ---- rotation ---- */

/** @brief Rotate log file by renaming to .1 suffix. */
static void rotate_log_files(void) {
    if (!g_logger.config.file_path) return;
    fclose(g_logger.fp);
    char old[512];
    snprintf(old, sizeof(old), "%s.1", g_logger.config.file_path);
    rename(g_logger.config.file_path, old);
    g_logger.fp = fopen(g_logger.config.file_path, "a");
    g_logger.current_size = 0;
}

/* ---- text-format output ---- */

/** @brief Format and write a plain-text log line.
 * @param lv Log level.
 * @param file Source file.
 * @param line Source line.
 * @param func Function name.
 * @param msg Log message.
 * @param msg_len Message length.
 * @return Number of bytes written. */
static int log_text(csilk_log_level_t lv, const char* file, int line,
                    const char* func, const char* msg, int msg_len) {
    const char* fn = strrchr(file, '/');
    fn = fn ? fn + 1 : file;

    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    int n = 0;
    if (g_logger.config.use_colors)
        n += fprintf(g_logger.fp, "%s %s%s\x1b[0m [%s:%d] %s(): ",
                     ts, level_colors[lv], level_names[lv], fn, line, func);
    else
        n += fprintf(g_logger.fp, "%s %s [%s:%d] %s(): ",
                     ts, level_names[lv], fn, line, func);
    n += (int)fwrite(msg, 1, (size_t)msg_len, g_logger.fp);
    n += fprintf(g_logger.fp, "\n");
    return n;
}

/* ---- JSON-format output (uses reflect) ---- */

/** @brief Build a cJSON object from log entry fields.
 * @param lv Log level.
 * @param file Source file.
 * @param line Source line.
 * @param func Function name.
 * @param msg Log message.
 * @param msg_len Message length.
 * @return cJSON object, or NULL on failure. */
static cJSON* build_json_entry(csilk_log_level_t lv, const char* file,
                                int line, const char* func,
                                const char* msg, int msg_len) {
    const char* fn = strrchr(file, '/');
    fn = fn ? fn + 1 : file;

    csilk_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.time_epoch = (int64_t)time(NULL);
    snprintf(entry.level, sizeof(entry.level), "%s", level_names[lv]);
    snprintf(entry.file,  sizeof(entry.file),  "%s", fn);
    entry.line = (int32_t)line;
    snprintf(entry.func,  sizeof(entry.func),  "%s", func);
    if (msg && msg_len > 0) {
        size_t cp = (size_t)msg_len < sizeof(entry.msg) - 1
                        ? (size_t)msg_len : sizeof(entry.msg) - 1;
        memcpy(entry.msg, msg, cp);
        entry.msg[cp] = '\0';
    }

    return cJSON_Parse(csilk_json_marshal("csilk_log_entry_t", &entry));
}

/** @brief Format and write a JSON-structured log line.
 * @param lv Log level.
 * @param file Source file.
 * @param line Source line.
 * @param func Function name.
 * @param extra Extra cJSON fields (takes ownership).
 * @param msg Log message.
 * @param msg_len Message length.
 * @return Number of bytes written. */
static int log_json(csilk_log_level_t lv, const char* file, int line,
                    const char* func, cJSON* extra,
                    const char* msg, int msg_len) {
    cJSON* root = build_json_entry(lv, file, line, func, msg, msg_len);
    if (!root) return 0;

    if (extra) {
        cJSON* child = extra->child;
        while (child) {
            cJSON* dupe = cJSON_Duplicate(child, 1);
            if (dupe) cJSON_AddItemToObject(root, child->string, dupe);
            child = child->next;
        }
        cJSON_Delete(extra);
    }

    char* line_str = cJSON_PrintUnformatted(root);
    int n = 0;
    if (line_str) {
        n = fprintf(g_logger.fp, "%s\n", line_str);
        free(line_str);
    }
    cJSON_Delete(root);
    return n;
}

/* ================================================================
 * public API
 * ================================================================ */

/** @brief Initialize the global logger with the given configuration. */
int csilk_log_init(csilk_log_config_t config) {
    if (g_logger.initialized) csilk_log_close();
    g_logger.config = config;
    if (config.file_path) {
        g_logger.fp = fopen(config.file_path, "a");
        if (!g_logger.fp) return -1;
        struct stat st;
        if (stat(config.file_path, &st) == 0)
            g_logger.current_size = (size_t)st.st_size;
    } else {
        g_logger.fp = stdout;
        g_logger.config.max_file_size = 0;
    }
    if (g_logger.config.use_colors == -1)
        g_logger.config.use_colors = isatty(fileno(g_logger.fp));
    if (uv_mutex_init(&g_logger.mutex) != 0) {
        if (config.file_path) fclose(g_logger.fp);
        return -1;
    }
    g_logger.initialized = 1;
    return 0;
}

/** @brief Internal: format and emit a log message (use macros instead). */
void _csilk_log_internal(csilk_log_level_t lv, const char* file, int line,
                         const char* func, const char* fmt, ...) {
    if (!g_logger.initialized || lv < g_logger.config.level) return;

    va_list args;
    va_start(args, fmt);
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    uv_mutex_lock(&g_logger.mutex);
    if (g_logger.config.max_file_size > 0 && g_logger.current_size >= g_logger.config.max_file_size)
        rotate_log_files();

    int n = g_logger.config.json_format
        ? log_json(lv, file, line, func, NULL, buf, len)
        : log_text(lv, file, line, func, buf, len);

    if (g_logger.config.file_path) g_logger.current_size += (size_t)n;
    uv_mutex_unlock(&g_logger.mutex);
}

/** @brief Internal: emit a structured JSON log entry with extra fields. */
void _csilk_log_structured(csilk_log_level_t lv, const char* file, int line,
                           const char* func, cJSON* extra,
                           const char* fmt, ...) {
    if (!g_logger.initialized || lv < g_logger.config.level) return;

    va_list args;
    va_start(args, fmt);
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    uv_mutex_lock(&g_logger.mutex);
    if (g_logger.config.max_file_size > 0 && g_logger.current_size >= g_logger.config.max_file_size)
        rotate_log_files();

    int n;
    if (g_logger.config.json_format) {
        n = log_json(lv, file, line, func, extra, buf, len);
    } else {
        if (extra) cJSON_Delete(extra);
        n = log_text(lv, file, line, func, buf, len);
    }

    if (g_logger.config.file_path) g_logger.current_size += (size_t)n;
    uv_mutex_unlock(&g_logger.mutex);
}

/** @brief Check whether the logger is in JSON format mode. */
int csilk_log_is_json(void) {
    return g_logger.initialized && g_logger.config.json_format;
}

/** @brief Build a simple key-value cJSON object for structured logging. */
cJSON* csilk_log_make_kv(const char* key, ...) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return NULL;
    va_list args;
    va_start(args, key);
    const char* k = key;
    while (k) {
        const char* v = va_arg(args, const char*);
        if (v) cJSON_AddStringToObject(obj, k, v);
        k = va_arg(args, const char*);
    }
    va_end(args);
    return obj;
}

/** @brief Close the global logger and release resources. */
void csilk_log_close(void) {
    if (!g_logger.initialized) return;
    uv_mutex_lock(&g_logger.mutex);
    if (g_logger.fp && g_logger.fp != stdout && g_logger.fp != stderr)
        fclose(g_logger.fp);
    g_logger.initialized = 0;
    uv_mutex_unlock(&g_logger.mutex);
    uv_mutex_destroy(&g_logger.mutex);
}
