/**
 * @file logger.c
 * @brief Thread-safe logging with rotation and source location.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uv.h>
#include "gin.h"

/** @brief Internal global logger state. */
typedef struct {
    gin_log_config_t config; /**< Logger configuration. */
    FILE* fp;                /**< Output file pointer (stdout or file). */
    size_t current_size;     /**< Current log file size for rotation. */
    uv_mutex_t mutex;        /**< Mutex for thread safety. */
    int initialized;         /**< Flag indicating logger is initialized. */
} gin_logger_t;

static gin_logger_t g_logger = { {0}, NULL, 0, {0}, 0 };

static const char* level_strings[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL" };
static const char* level_colors[] = { "\x1b[35m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[41;1m" };

/** @brief Rotate log file (rename current to .1, open new file). */
static void rotate_log_files() {
    if (!g_logger.config.file_path) return;
    
    fclose(g_logger.fp);
    
    char old_name[512];
    snprintf(old_name, sizeof(old_name), "%s.1", g_logger.config.file_path);
    rename(g_logger.config.file_path, old_name);
    
    g_logger.fp = fopen(g_logger.config.file_path, "a");
    g_logger.current_size = 0;
}

int gin_log_init(gin_log_config_t config) {
    if (g_logger.initialized) gin_log_close();

    g_logger.config = config;
    if (config.file_path) {
        g_logger.fp = fopen(config.file_path, "a");
        if (!g_logger.fp) return -1;
        
        struct stat st;
        if (stat(config.file_path, &st) == 0) {
            g_logger.current_size = (size_t)st.st_size;
        }
    } else {
        g_logger.fp = stdout;
        g_logger.config.max_file_size = 0; // No rotation for stdout
    }

    if (g_logger.config.use_colors == -1) {
        g_logger.config.use_colors = isatty(fileno(g_logger.fp));
    }

    if (uv_mutex_init(&g_logger.mutex) != 0) {
        if (config.file_path) fclose(g_logger.fp);
        return -1;
    }

    g_logger.initialized = 1;
    return 0;
}

void _gin_log_internal(gin_log_level_t level, const char* file, int line, const char* func, const char* fmt, ...) {
    if (!g_logger.initialized || level < g_logger.config.level) return;

    uv_mutex_lock(&g_logger.mutex);

    // Check for rotation
    if (g_logger.config.max_file_size > 0 && g_logger.current_size >= g_logger.config.max_file_size) {
        rotate_log_files();
    }

    // Time
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    // Extract filename only from path
    const char* filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    int bytes_written = 0;

    // Header
    if (g_logger.config.use_colors) {
        bytes_written += fprintf(g_logger.fp, "%s %s%s\x1b[0m [%s:%d] %s(): ", 
                                 time_buf, level_colors[level], level_strings[level], 
                                 filename, line, func);
    } else {
        bytes_written += fprintf(g_logger.fp, "%s %s [%s:%d] %s(): ", 
                                 time_buf, level_strings[level], 
                                 filename, line, func);
    }

    // Body
    va_list args;
    va_start(args, fmt);
    bytes_written += vfprintf(g_logger.fp, fmt, args);
    va_end(args);

    bytes_written += fprintf(g_logger.fp, "\n");
    fflush(g_logger.fp);

    if (g_logger.config.file_path) {
        g_logger.current_size += (size_t)bytes_written;
    }

    uv_mutex_unlock(&g_logger.mutex);
}

void gin_log_close() {
    if (!g_logger.initialized) return;
    uv_mutex_lock(&g_logger.mutex);
    if (g_logger.fp && g_logger.fp != stdout && g_logger.fp != stderr) {
        fclose(g_logger.fp);
    }
    g_logger.initialized = 0;
    uv_mutex_unlock(&g_logger.mutex);
    uv_mutex_destroy(&g_logger.mutex);
}
