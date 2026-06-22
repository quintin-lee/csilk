/**
 * @file logger.c
 * @brief Thread-safe structured logger with JSON and human-readable output,
 *        file rotation, and ANSI color support.
 *
 * === Design ===
 *
 * The logger is a global singleton (g_logger) protected by a mutex for
 * thread-safe access. Two output modes are available:
 *
 *   Text mode (default):
 *     [2024-01-15 10:30:00] INFO  [file.c:42] function(): <request_id> message
 *     ANSI color codes are added for each level when use_colors is enabled.
 *
 *   JSON mode:
 *     {"time_epoch":1705312200,"level":"INFO","request_id":"...",
 *      "file":"file.c","line":42,"func":"function","msg":"..."}
 *     Uses the csilk reflection engine (CSILK_REGISTER_REFLECT) for automatic
 *     struct-to-JSON serialization, avoiding manual JSON string building.
 *
 * === Thread Safety ===
 *
 * All public log macros (CSILK_LOG_I, CSILK_LOG_E, etc.) acquire
 * g_logger.mutex before writing. The thread-local request ID (tl_request_id)
 * allows each thread to track its own request context without contention.
 *
 * === File Rotation ===
 *
 * When max_file_size is set and the current file exceeds it, the logger
 * renames <path> to <path>.1 (single-backup rotation) and opens a new file.
 * Rotation happens inline during log write, protected by the mutex.
 *
 * === Log Levels ===
 *
 *   TRACE (0) - Most verbose, for debugging internals
 *   DEBUG (1) - Detailed information for developers
 *   INFO  (2) - Normal operational messages (default)
 *   WARN  (3) - Unexpected but handled situations
 *   ERROR (4) - Errors that don't stop the server
 *   FATAL (5) - Critical errors causing shutdown
 *
 * The level filter is checked inside each log macro call before any formatting
 * or I/O occurs, so disabled levels have near-zero overhead.
 * @copyright MIT License
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/reflection/reflect.h"

/* ---- reflectable log-entry struct ---- */

/** @brief Structured log entry type used for JSON-formatted log output.
 *
 * All fields are fixed-size character arrays to avoid dynamic allocation.
 * The type is registered with the reflection engine for automatic JSON
 * serialization via csilk_json_marshal(). */
typedef struct csilk_log_entry_s {
	int64_t time_epoch;  /**< unix timestamp */
	char level[8];	     /**< TRACE/DEBUG/INFO/WARN/ERROR/FATAL */
	char request_id[CSILK_UUID_BUF_SIZE]; /**< unique request id */
	char file[64];	     /**< source filename */
	int32_t line;	     /**< source line number */
	char func[64];	     /**< function name */
	char msg[1024];	     /**< log message */
} csilk_log_entry_t;

#define LOG_ENTRY_MAP(X)                                                                           \
	X(csilk_log_entry_t, time_epoch, CSILK_TYPE_INT64, sizeof(int64_t), 0, false, nullptr)     \
	X(csilk_log_entry_t, level, CSILK_TYPE_STRING, 8, 0, false, nullptr)                       \
	X(csilk_log_entry_t, request_id, CSILK_TYPE_STRING, CSILK_UUID_BUF_SIZE, 0, false, nullptr)                 \
	X(csilk_log_entry_t, file, CSILK_TYPE_STRING, 64, 0, false, nullptr)                       \
	X(csilk_log_entry_t, line, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)           \
	X(csilk_log_entry_t, func, CSILK_TYPE_STRING, 64, 0, false, nullptr)                       \
	X(csilk_log_entry_t, msg, CSILK_TYPE_STRING, 1024, 0, false, nullptr)

CSILK_REGISTER_REFLECT(csilk_log_entry_t, LOG_ENTRY_MAP)

/* ---- internal logger state ---- */

/** @brief Internal logger singleton — holds configuration, file pointer, mutex,
 * and current file size. */
typedef struct {
	csilk_log_config_t config; /**< Logger configuration. */
	FILE* fp;		   /**< Output file pointer. */
	size_t current_size;	   /**< Current log file size. */
	uv_mutex_t mutex;	   /**< Mutex for thread-safe logging. */
	int initialized;	   /**< Whether logger is initialized. */
} csilk_logger_t;

static csilk_logger_t g_logger = {{0}, nullptr, 0, {0}, 0};

static _Thread_local char tl_request_id[CSILK_UUID_BUF_SIZE];

static char*
get_tl_request_id(void)
{
	return tl_request_id;
}

static const char* level_names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
static const char* level_colors[] = {
    "\x1b[35m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[41;1m"};

/* ---- rotation ---- */

/** @brief Rotate the current log file by renaming it with a ".1" suffix.
 *
 * Closes the current file, renames "<path>" to "<path>.1", opens a new
 * file at the original path in append mode, and resets the current_size
 * counter. This is a simple single-backup rotation (not multi-generational).
 *
 * @note Only called when g_logger.config.max_file_size is exceeded.
 * @note Not thread-safe on its own; the caller must hold g_logger.mutex. */
static void
rotate_log_files(void)
{
	if (!g_logger.config.file_path) {
		return;
	}
	fclose(g_logger.fp);
	char old[512];
	snprintf(old, sizeof(old), "%s.1", g_logger.config.file_path);
	rename(g_logger.config.file_path, old);
	g_logger.fp = fopen(g_logger.config.file_path, "a");
	g_logger.current_size = 0;
}

/* ---- text-format output ---- */

/** @brief Format and write a human-readable plain-text log line.
 *
 * Produces output like:
 *   "2024-01-15 10:30:00 INFO  [file.c:42] function(): <request_id> message"
 * ANSI color codes are added when use_colors is enabled. Thread-local
 * request ID is appended if set via csilk_log_set_request_id().
 *
 * @param lv      Log level enum (controls coloring/level label).
 * @param file    Source file name (only basename is used).
 * @param line    Source line number.
 * @param func    Function name.
 * @param msg     Log message content (not null-terminated).
 * @param msg_len Length of the message content.
 * @return Number of bytes written to g_logger.fp. */
static int
log_text(csilk_log_level_t lv,
	 const char* file,
	 int line,
	 const char* func,
	 const char* msg,
	 int msg_len)
{
	const char* fn = strrchr(file, '/');
	fn = fn ? fn + 1 : file;

	char ts[32];
	time_t now = time(nullptr);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

	int n = 0;
	if (g_logger.config.use_colors) {
		n += fprintf(g_logger.fp,
			     "%s %s%s\x1b[0m [%s:%d] %s(): ",
			     ts,
			     level_colors[lv],
			     level_names[lv],
			     fn,
			     line,
			     func);
	} else {
		n += fprintf(
		    g_logger.fp, "%s %s [%s:%d] %s(): ", ts, level_names[lv], fn, line, func);
	}

	char* tl_request_id = get_tl_request_id();
	if (tl_request_id[0] != '\0') {
		n += fprintf(g_logger.fp, "<%s> ", tl_request_id);
	}

	n += (int)fwrite(msg, 1, (size_t)msg_len, g_logger.fp);
	n += fprintf(g_logger.fp, "\n");
	return n;
}

/* ---- JSON-format output (uses reflect) ---- */

/** @brief Build a cJSON object from log entry fields using the reflection
 * engine.
 *
 * Populates a csilk_log_entry_t struct with the provided metadata, then
 * serializes it to JSON via csilk_json_marshal() and parses the result back
 * into a cJSON object. This round-trip is used to produce consistent JSON
 * output that matches the registered reflection schema.
 *
 * @param lv      Log level.
 * @param file    Source file name.
 * @param line    Source line number.
 * @param func    Function name.
 * @param msg     Log message content.
 * @param msg_len Message length (may truncate to fit the entry struct).
 * @return cJSON object ready for merging extra fields, or nullptr on failure.
 * @note The returned cJSON must be freed by the caller with cJSON_Delete(). */
static cJSON*
build_json_entry(csilk_log_level_t lv,
		 const char* file,
		 int line,
		 const char* func,
		 const char* msg,
		 int msg_len)
{
	const char* fn = strrchr(file, '/');
	fn = fn ? fn + 1 : file;

	csilk_log_entry_t entry;
	memset(&entry, 0, sizeof(entry));
	entry.time_epoch = (int64_t)time(nullptr);
	snprintf(entry.level, sizeof(entry.level), "%s", level_names[lv]);
	char* tl_request_id = get_tl_request_id();
	snprintf(entry.request_id, sizeof(entry.request_id), "%s", tl_request_id);
	snprintf(entry.file, sizeof(entry.file), "%s", fn);
	entry.line = (int32_t)line;
	snprintf(entry.func, sizeof(entry.func), "%s", func);
	if (msg && msg_len > 0) {
		size_t cp = (size_t)msg_len < sizeof(entry.msg) - 1 ? (size_t)msg_len
								    : sizeof(entry.msg) - 1;
		memcpy(entry.msg, msg, cp);
		entry.msg[cp] = '\0';
	}

	char* tmp = csilk_json_marshal("csilk_log_entry_t", &entry);
	cJSON* result = tmp ? cJSON_Parse(tmp) : nullptr;
	free(tmp);
	return result;
}

/** @brief Format and write a structured JSON log line with optional extra
 * fields.
 *
 * Builds the base log entry via build_json_entry(), merges any extra cJSON
 * fields (the @p extra object's children are duplicated into the root),
 * serializes to a compact JSON string, and writes it to the output.
 *
 * @param lv      Log level.
 * @param file    Source file name.
 * @param line    Source line number.
 * @param func    Function name.
 * @param extra   Extra cJSON object with additional key-value pairs to merge
 *                into the log entry. Ownership is taken (cJSON_Delete is
 * called). May be nullptr.
 * @param msg     Log message content.
 * @param msg_len Message length.
 * @return Number of bytes written, or 0 on failure. */
static int
log_json(csilk_log_level_t lv,
	 const char* file,
	 int line,
	 const char* func,
	 cJSON* extra,
	 const char* msg,
	 int msg_len)
{
	cJSON* root = build_json_entry(lv, file, line, func, msg, msg_len);
	if (!root) {
		return 0;
	}

	if (extra) {
		cJSON* child = extra->child;
		while (child) {
			cJSON* dupe = cJSON_Duplicate(child, 1);
			if (dupe) {
				cJSON_AddItemToObject(root, child->string, dupe);
			}
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

/* ================================================================
 * public API
 * ================================================================ */

/** @brief Initialize (or reinitialize) the global logger with the given
 * configuration.
 *
 * Configures the output destination (stdout if no file_path, or a file if
 * set), the minimum log level, coloring (auto-detected for terminals when
 * use_colors is -1), and whether to use structured JSON format. If the logger
 * was previously initialized, csilk_log_close() is called first. A mutex is
 * initialized for thread-safe operation.
 *
 * @param config Logger configuration struct with desired settings.
 * @return 0 on success, -1 if the file could not be opened or mutex init fails.
 * @note If file_path is nullptr, output goes to stdout and max_file_size is
 *       effectively ignored (set to 0 internally). */
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
		}
	} else {
		g_logger.fp = stdout;
		g_logger.config.max_file_size = 0;
	}
	if (g_logger.config.use_colors == -1) {
		g_logger.config.use_colors = isatty(fileno(g_logger.fp));
	}
	if (uv_mutex_init(&g_logger.mutex) != 0) {
		if (config.file_path) {
			fclose(g_logger.fp);
		}
		return -1;
	}
	g_logger.initialized = 1;
	return 0;
}

/** @brief Internal: format and emit a log message to the global logger.
 *
 * Formats the variadic message via vsnprintf, acquires the logger mutex,
 * checks file rotation (if file logging and max_file_size exceeded), and
 * writes the entry as either JSON or plain text depending on configuration.
 *
 * @param lv   Log severity level (filtered against g_logger.config.level).
 * @param file Source file name (provided by CSILK_LOG_* macro).
 * @param line Source line number (provided by CSILK_LOG_* macro).
 * @param func Function name (provided by CSILK_LOG_* macro).
 * @param fmt  printf-style format string.
 * @param ...  Variadic arguments for the format string.
 * @note Use the CSILK_LOG_* macros (CSILK_LOG_I, CSILK_LOG_E, etc.) instead
 *       of calling this function directly. The macros automatically supply
 *       __FILE__, __LINE__, and __func__. */
CSILK_INTERNAL void
_csilk_log_internal(
    csilk_log_level_t lv, const char* file, int line, const char* func, const char* fmt, ...)
{
	if (!g_logger.initialized || lv < g_logger.config.level) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	char buf[4096];
	int len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (len < 0) {
		len = 0;
	}
	if (len >= (int)sizeof(buf)) {
		len = (int)sizeof(buf) - 1;
	}

	uv_mutex_lock(&g_logger.mutex);
	if (g_logger.config.max_file_size > 0 &&
	    g_logger.current_size >= g_logger.config.max_file_size) {
		rotate_log_files();
	}

	int n = g_logger.config.json_format ? log_json(lv, file, line, func, nullptr, buf, len)
					    : log_text(lv, file, line, func, buf, len);

	if (g_logger.config.file_path) {
		g_logger.current_size += (size_t)n;
	}
	uv_mutex_unlock(&g_logger.mutex);
}

/** @brief Internal: emit a structured JSON log entry with extra key-value
 * fields.
 *
 * Like _csilk_log_internal() but accepts an additional cJSON object of extra
 * fields. In JSON mode, the extra fields are merged into the output. In text
 * mode, the extra fields are discarded (cJSON_Delete is called).
 *
 * @param lv   Log severity level.
 * @param file Source file name.
 * @param line Source line number.
 * @param func Function name.
 * @param extra cJSON object of extra fields to include (ownership taken).
 * @param fmt  printf-style format string.
 * @param ...  Variadic arguments.
 * @note Use the CSILK_LOG_KV macro instead of calling this directly. */
CSILK_INTERNAL void
_csilk_log_structured(csilk_log_level_t lv,
		      const char* file,
		      int line,
		      const char* func,
		      cJSON* extra,
		      const char* fmt,
		      ...)
{
	if (!g_logger.initialized || lv < g_logger.config.level) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	char buf[4096];
	int len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (len < 0) {
		len = 0;
	}
	if (len >= (int)sizeof(buf)) {
		len = (int)sizeof(buf) - 1;
	}

	uv_mutex_lock(&g_logger.mutex);
	if (g_logger.config.max_file_size > 0 &&
	    g_logger.current_size >= g_logger.config.max_file_size) {
		rotate_log_files();
	}

	int n;
	if (g_logger.config.json_format) {
		n = log_json(lv, file, line, func, extra, buf, len);
	} else {
		if (extra) {
			cJSON_Delete(extra);
		}
		n = log_text(lv, file, line, func, buf, len);
	}

	if (g_logger.config.file_path) {
		g_logger.current_size += (size_t)n;
	}
	uv_mutex_unlock(&g_logger.mutex);
}

/** @brief Check whether the global logger is configured for structured JSON
 * output.
 *
 * @return 1 if the logger is initialized and json_format is enabled, 0
 * otherwise.
 * @note Useful for handlers that want to produce consistent log output
 *       format matching the global setting. */
int
csilk_log_is_json(void)
{
	return g_logger.initialized && g_logger.config.json_format;
}

/** @brief Set the Request ID for the current thread.
 *
 * Stores the request ID in thread-local storage, allowing subsequent log
 * calls on the same thread to automatically include it without passing
 * the context explicitly. */
void
csilk_log_set_request_id(const char* request_id)
{
	char* tl_request_id = get_tl_request_id();
	if (request_id) {
		snprintf(tl_request_id, CSILK_UUID_BUF_SIZE, "%s", request_id);
	} else {
		tl_request_id[0] = '\0';
	}
}

/** @brief Build a key-value cJSON object for structured logging.
 *
 * Helper to create a flat JSON object from a nullptr-terminated list of strings.
 * Used primarily with CSILK_LOG_KV. */
cJSON*
csilk_log_make_kv(const char* key, ...)
{
	cJSON* obj = cJSON_CreateObject();
	if (!obj) {
		return nullptr;
	}
	va_list args;
	va_start(args, key);
	const char* k = key;
	while (k) {
		const char* v = va_arg(args, const char*);
		if (v) {
			cJSON_AddStringToObject(obj, k, v);
		}
		k = va_arg(args, const char*);
	}
	va_end(args);
	return obj;
}

/** @brief Close the global logger and release resources.
 *
 * Closes file handles and destroys mutexes. Safe to call multiple times. */
void
csilk_log_close(void)
{
	if (!g_logger.initialized) {
		return;
	}
	uv_mutex_lock(&g_logger.mutex);
	if (g_logger.fp && g_logger.fp != stdout && g_logger.fp != stderr) {
		fclose(g_logger.fp);
	}
	g_logger.initialized = 0;
	uv_mutex_unlock(&g_logger.mutex);
	uv_mutex_destroy(&g_logger.mutex);
}
