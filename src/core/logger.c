/**
 * @file logger.c
 * @brief Thread-safe logging with JSON structured output, file rotation, ANSI colors.
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

typedef struct {
    csilk_log_config_t config;
    FILE*              fp;
    size_t             current_size;
    uv_mutex_t         mutex;
    int                initialized;
} csilk_logger_t;

static csilk_logger_t g_logger = { {0}, NULL, 0, {0}, 0 };

static const char* level_strings[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};
static const char* level_colors[] = {
    "\x1b[35m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[41;1m"
};

/* ---- helper: print a JSON-safe escaped string ---- */
static int fwrite_json_str(FILE* fp, const char* s) {
    int n = 0;
    n += fputc('"', fp);
    for (; s && *s; s++) {
        switch (*s) {
        case '"':  n += fprintf(fp, "\\\""); break;
        case '\\': n += fprintf(fp, "\\\\"); break;
        case '\n': n += fprintf(fp, "\\n");  break;
        case '\r': n += fprintf(fp, "\\r");  break;
        case '\t': n += fprintf(fp, "\\t");  break;
        default:   n += fputc(*s, fp);       break;
        }
    }
    n += fputc('"', fp);
    return n;
}

/* ---- internal: write text-format log line ---- */
static int log_write_text(csilk_log_level_t level, const char* file, int line,
                          const char* func, const char* msg, int msg_len) {
    const char* filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    char time_buf[32];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    int n = 0;
    if (g_logger.config.use_colors) {
        n += fprintf(g_logger.fp, "%s %s%s\x1b[0m [%s:%d] %s(): ",
                     time_buf, level_colors[level], level_strings[level],
                     filename, line, func);
    } else {
        n += fprintf(g_logger.fp, "%s %s [%s:%d] %s(): ",
                     time_buf, level_strings[level],
                     filename, line, func);
    }
    n += (int)fwrite(msg, 1, (size_t)msg_len, g_logger.fp);
    n += fprintf(g_logger.fp, "\n");
    fflush(g_logger.fp);
    return n;
}

/* ---- internal: write JSON-format log line ---- */
static int log_write_json(csilk_log_level_t level, const char* file, int line,
                          const char* func, cJSON* extra,
                          const char* msg, int msg_len) {
    const char* filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    cJSON* root = extra ? extra : cJSON_CreateObject();
    if (!root) return 0;

    char ts[32];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_info);

    cJSON_AddStringToObject(root, "time",  ts);
    cJSON_AddStringToObject(root, "level", level_strings[level]);
    cJSON_AddStringToObject(root, "file",  filename);
    cJSON_AddNumberToObject(root, "line",  (double)line);
    cJSON_AddStringToObject(root, "func",  func);
    if (msg && msg_len > 0) {
        char* msg_buf = malloc((size_t)(msg_len + 1));
        if (msg_buf) {
            memcpy(msg_buf, msg, (size_t)msg_len);
            msg_buf[msg_len] = '\0';
            cJSON_AddStringToObject(root, "msg", msg_buf);
            free(msg_buf);
        }
    }

    char* line_str = cJSON_PrintUnformatted(root);
    int n = 0;
    if (line_str) {
        n = fprintf(g_logger.fp, "%s\n", line_str);
        free(line_str);
    }
    fflush(g_logger.fp);
    cJSON_Delete(root);
    return n;
}

/* ---- rotation ---- */
static void rotate_log_files(void) {
    if (!g_logger.config.file_path) return;
    fclose(g_logger.fp);
    char old[512];
    snprintf(old, sizeof(old), "%s.1", g_logger.config.file_path);
    rename(g_logger.config.file_path, old);
    g_logger.fp = fopen(g_logger.config.file_path, "a");
    g_logger.current_size = 0;
}

/* ================================================================
 * public API
 * ================================================================ */

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

void _csilk_log_internal(csilk_log_level_t level, const char* file, int line,
                         const char* func, const char* fmt, ...) {
    if (!g_logger.initialized || level < g_logger.config.level) return;

    va_list args;
    va_start(args, fmt);
    char msg_buf[4096];
    int msg_len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);
    if (msg_len < 0) msg_len = 0;
    if (msg_len >= (int)sizeof(msg_buf)) msg_len = (int)sizeof(msg_buf) - 1;

    uv_mutex_lock(&g_logger.mutex);

    if (g_logger.config.max_file_size > 0 &&
        g_logger.current_size >= g_logger.config.max_file_size)
        rotate_log_files();

    int n;
    if (g_logger.config.json_format) {
        n = log_write_json(level, file, line, func, NULL, msg_buf, msg_len);
    } else {
        n = log_write_text(level, file, line, func, msg_buf, msg_len);
    }

    if (g_logger.config.file_path)
        g_logger.current_size += (size_t)n;

    uv_mutex_unlock(&g_logger.mutex);
}

void _csilk_log_structured(csilk_log_level_t level, const char* file, int line,
                           const char* func, cJSON* extra,
                           const char* fmt, ...) {
    if (!g_logger.initialized || level < g_logger.config.level) return;

    va_list args;
    va_start(args, fmt);
    char msg_buf[4096];
    int msg_len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);
    if (msg_len < 0) msg_len = 0;
    if (msg_len >= (int)sizeof(msg_buf)) msg_len = (int)sizeof(msg_buf) - 1;

    uv_mutex_lock(&g_logger.mutex);

    if (g_logger.config.max_file_size > 0 &&
        g_logger.current_size >= g_logger.config.max_file_size)
        rotate_log_files();

    int n;
    if (g_logger.config.json_format) {
        n = log_write_json(level, file, line, func, extra, msg_buf, msg_len);
    } else {
        if (extra) cJSON_Delete(extra);
        n = log_write_text(level, file, line, func, msg_buf, msg_len);
    }

    if (g_logger.config.file_path)
        g_logger.current_size += (size_t)n;

    uv_mutex_unlock(&g_logger.mutex);
}

int csilk_log_is_json(void) {
    return g_logger.initialized && g_logger.config.json_format;
}

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

void csilk_log_close(void) {
    if (!g_logger.initialized) return;
    uv_mutex_lock(&g_logger.mutex);
    if (g_logger.fp && g_logger.fp != stdout && g_logger.fp != stderr)
        fclose(g_logger.fp);
    g_logger.initialized = 0;
    uv_mutex_unlock(&g_logger.mutex);
    uv_mutex_destroy(&g_logger.mutex);
}
