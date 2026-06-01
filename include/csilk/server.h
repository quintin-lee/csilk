/**
 * @file server.h
 * @brief Server lifecycle: creation, configuration, execution, and shutdown.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_SERVER_H
#define CSILK_SERVER_H

#include "csilk/router.h"
#include "csilk/middleware.h"
#include "csilk/hooks.h"

/**
 * @brief Create a new server instance.
 *
 * Allocates and initialises a server bound to the given router.  The server
 * takes ownership of the router and frees it in csilk_server_free.
 *
 * @param router The router to use for request dispatch.  Must not be nullptr.
 * @return A new server instance, or nullptr on allocation failure.
 */
csilk_server_t* csilk_server_new(csilk_router_t* router);

/**
 * @brief Register global middleware.
 *
 * Middleware is executed for every request in the order it was added.  The
 * handler array has a fixed maximum size (typically 64).
 *
 * @param server  Server instance.
 * @param handler Middleware function.
 * @return 0 on success, -1 if the internal handler array is full.
 */
int csilk_server_use(csilk_server_t* server, csilk_handler_t handler);

/**
 * @brief Set a custom handler for 404 (route-not-found) responses.
 *
 * Replaces the default 404 behaviour.  The handler is invoked with the
 * request context (status 404 is NOT pre-set — the handler may set its own).
 * Pass nullptr to restore the default 404 handler.
 *
 * @param server  Server instance.
 * @param handler Handler function, or nullptr for default.
 */
void csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler);

/**
 * @brief Enable single-page application (SPA) fallback mode.
 *
 * Unmatched GET requests serve index.html from @p doc_root instead of
 * returning 404.  Overrides any custom 404 handler.  Useful for serving
 * React/Vue/Angular SPAs where the router handles URLs client-side.
 *
 * @param server   Server instance.
 * @param doc_root Directory containing index.html.  The path is copied
 *                 internally.
 */
void csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root);

/**
 * @brief Destroy the server and release all resources.
 *
 * Stops the server if running, closes all connections, and frees the
 * router, hooks, and internal structures.
 *
 * @param server Server instance to free.
 */
void csilk_server_free(csilk_server_t* server);

/**
 * @brief Request a graceful server shutdown.
 *
 * Signals the event loop to stop after all active requests complete.
 * New connections are refused.
 *
 * @param server Server instance.
 */
void csilk_server_stop(csilk_server_t* server);
void csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn);

/**
 * @brief Apply server configuration options.
 *
 * Copies values from @p config into the server's internal state.  Should
 * be called before csilk_server_run.  The config struct may be stack-allocated.
 *
 * @param server Server instance.
 * @param config Pointer to the configuration to apply.
 */
void csilk_server_set_config(csilk_server_t* server, const csilk_server_config_t* config);

/**
 * @brief Set the maximum number of concurrent connections and return the
 * previous limit.
 *
 * @param server Server instance.
 * @param max    New limit (0 = unlimited).
 * @return The previous maximum connections value.
 */
int csilk_server_set_max_connections(csilk_server_t* server, int max);

/**
 * @brief Set the global crypto driver for the server.
 *
 * Replaces the default software crypto routines with a user-provided
 * implementation.  Pass nullptr to restore the built-in defaults.
 *
 * @param server The server instance.
 * @param driver Pointer to a csilk_crypto_driver_t, or nullptr for defaults.
 *               The driver struct must remain valid for the server's lifetime.
 */
void csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver);

/**
 * @brief Set the cipher driver for symmetric/asymmetric encryption.
 *
 * Replaces the default OpenSSL-based AES-256-GCM / RSA-OAEP / RSA-PSS
 * implementations with a user-provided driver.  Pass nullptr to restore the
 * built-in defaults.
 *
 * @param server The server instance.
 * @param driver Pointer to a csilk_cipher_driver_t, or nullptr for defaults.
 *               The driver struct must remain valid for the server's lifetime.
 */
void csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver);

/**
 * @brief Replace the context key-value storage driver.
 *
 * @param server Server instance.
 * @param driver Pointer to the new driver, or nullptr to restore the default
 *               in-memory arena-backed driver.  The driver struct must remain
 *               valid for the server's lifetime.
 */
void csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver);

/**
 * @brief Start the server and enter the libuv event loop.
 *
 * This call blocks until the server is stopped (csilk_server_stop) or a
 * fatal error occurs.
 *
 * @param server Server instance.
 * @param port   TCP port to listen on.
 * @return 0 on normal shutdown, -1 on initialisation failure.
 */
int csilk_server_run(csilk_server_t* server, int port);

/** @brief Get the router instance attached to a server.
 *  @param server The server instance.
 *  @return Pointer to csilk_router_t. */
csilk_router_t* csilk_server_get_router(csilk_server_t* server);

/** @brief Swap the router instance attached to a server (for hot-reloading).
 *  Frees the old router and attaches the new one. Thread-safe if called
 *  from the main event loop thread (e.g. during a hot-reload callback).
 *  @param server The server instance.
 *  @param router The new router instance. */
void csilk_server_set_router(csilk_server_t* server, csilk_router_t* router);

/**
 * @brief Get the client's IP address.
 *
 * Checks the X-Forwarded-For / X-Real-IP headers first (if present), then
 * falls back to the socket peer address.
 *
 * @param c  The request context.
 * @return A NUL-terminated IP string, or nullptr if the address cannot be
 *         determined.  Valid until csilk_ctx_cleanup.
 */
const char* csilk_get_client_ip(csilk_ctx_t* c);

/* --- Database Interface --- */

/**
 * @brief Initialise the database subsystem.
 *
 * Registers built-in drivers (SQLite3, etc.).  Must be called once before
 * any csilk_db_pool_new call.  Safe to call multiple times.
 */
void csilk_db_init(void);

/**
 * @brief Create a new database connection pool.
 *
 * The pool maintains a single connection (or opens a new one on demand).
 * All database operations go through the pool, which provides mutex-based
 * thread safety.
 *
 * @param driver_name  Driver identifier (e.g., "sqlite").  Must have been
 *                     registered via csilk_db_register_driver or the built-in
 *                     init.
 * @param dsn          Data source name (driver-specific, e.g., "file:test.db").
 * @return A new pool instance, or nullptr if the driver is unknown or connection
 *         fails.
 */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/**
 * @brief Free a database pool and disconnect.
 *
 * Closes the underlying connection and frees the pool struct.
 *
 * @param pool  The pool to free.  Must not be nullptr.
 */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/**
 * @brief Execute a SELECT query and return the result as a JSON array.
 *
 * Each row becomes a JSON object keyed by column name.
 *
 * @param pool  Connection pool.
 * @param sql   SQL SELECT statement.
 * @return A cJSON array of row objects (caller must free with cJSON_Delete),
 *         or nullptr on failure.
 */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a statement that returns no result rows.
 *
 * Suitable for INSERT, UPDATE, DELETE, CREATE TABLE, etc.
 *
 * @param pool  Connection pool.
 * @param sql   SQL statement.
 * @return 0 on success, -1 on failure.
 */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders.
 *
 * Each ? in @p sql is replaced with the corresponding value from @p params
 * (The driver handles escaping internally).  The result is returned as a
 * JSON array.
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params nullptr-terminated array of string values for the placeholders.
 *               The array must end with a nullptr sentinel.
 * @return A cJSON array (caller must free), or nullptr on failure.
 */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params);

/* --- Logging API --- */

/** @brief Initialize the global logger with config.
 * @param config Logger configuration.
 * @return 0 on success, -1 on failure. */
int csilk_log_init(csilk_log_config_t config);

/** @brief Internal log function (use macros instead).
 * @param lv Log severity level.
 * @param file Source file name (__FILE__).
 * @param line Source line number (__LINE__).
 * @param func Function name (__func__).
 * @param fmt Printf-style format string.
 * @param ... Format arguments. */
void _csilk_log_internal(
    csilk_log_level_t lv, const char* file, int line, const char* func, const char* fmt, ...);

/** @brief Close the global logger. */
void csilk_log_close();

/** @brief Log a structured JSON message with extra key-value fields.
 *
 *  Produces a JSON log line with the standard fields
 *  (time/level/file/line/func/msg) plus any fields in @p extra.  If json_format
 *  is off this behaves like a normal log line (extra fields are ignored).
 *
 *  @param lv    Log severity level.
 *  @param file    Source file name (__FILE__).
 *  @param line    Source line number (__LINE__).
 *  @param func    Function name (__func__).
 *  @param extra   cJSON object with extra structured fields (can be nullptr).
 *                 Ownership is taken — do not use after the call.
 *  @param fmt     Printf-style format string for the log message.
 *  @param ...     Format arguments. */
void _csilk_log_structured(csilk_log_level_t lv,
			   const char* file,
			   int line,
			   const char* func,
			   cJSON* extra,
			   const char* fmt,
			   ...);

/** @brief Check whether the logger is in JSON format mode.
 * @return 1 if json_format is enabled, 0 otherwise. */
int csilk_log_is_json(void);

/** @brief Set the Request ID for the current thread (for log correlation).
 * @param request_id The Request ID string, or nullptr to clear. */
void csilk_log_set_request_id(const char* request_id);

/** @brief Create a simple key-value cJSON object for structured logging.
 *
 *  Convenience helper that builds a cJSON object from alternating key/value
 *  string pairs terminated by a nullptr key.
 *
 *  @code
 *    cJSON* fields = csilk_log_make_kv("method", method, "path", path, nullptr);
 *    _csilk_log_structured(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, fields,
 *                          "request completed");
 *  @endcode
 *
 *  @param key   First key.
 *  @param ...   Value, then key, value, ... terminated by nullptr.
 *  @return New cJSON object (caller owns). */
cJSON* csilk_log_make_kv(const char* key, ...);

/** @name Logging Macros
 *  Convenience macros that capture source location.
 *  @{ */
/** @brief Log a TRACE-level message. */
#define CSILK_LOG_T(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a DEBUG-level message. */
#define CSILK_LOG_D(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log an INFO-level message. */
#define CSILK_LOG_I(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a WARN-level message. */
#define CSILK_LOG_W(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log an ERROR-level message. */
#define CSILK_LOG_E(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
/** @brief Log a FATAL-level message. */
#define CSILK_LOG_F(...)                                                                           \
	_csilk_log_internal(CSILK_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

/** @brief Log a structured JSON message (only meaningful when json_format is
 *  on).
 *  @param level Log level.
 *  @param extra  cJSON* with extra fields (can be nullptr).
 *  @param ...    printf-style format and args for the message string. */
#define CSILK_LOG_STRUCT(level, extra, ...)                                                        \
	_csilk_log_structured(level, __FILE__, __LINE__, __func__, extra, __VA_ARGS__)
/** @} */

/* --- Arena API --- */

/**
 * @brief Create a new arena allocator.
 *
 * The arena allocates memory in fixed-size chunks and hands out bump-allocated
 * blocks.  When the current chunk is exhausted a new one is allocated.
 *
 * @param default_chunk_size  Initial chunk size in bytes.  Pass 0 for a
 *                            sensible default (typically 4–8 KB).
 * @return Pointer to the new arena, or nullptr if malloc fails.
 */
csilk_arena_t* csilk_arena_new(size_t default_chunk_size);

/**
 * @brief Allocate zero-initialised memory from an arena.
 *
 * The returned memory is valid until csilk_arena_free, csilk_arena_reset, or
 * csilk_ctx_cleanup.  No individual free() is required.
 *
 * @param arena  The arena allocator.
 * @param size   Number of bytes to allocate.
 * @return Pointer to the allocated block (always suitably aligned), or nullptr
 *         if the allocation failed (the arena's malloc failed).
 */
void* csilk_arena_alloc(csilk_arena_t* arena, size_t size);

/**
 * @brief Duplicate a NUL-terminated string using the arena allocator.
 *
 * @param arena  The arena allocator.
 * @param s      Source string to duplicate.  Must be NUL-terminated.
 * @return A copy of @p s allocated from @p arena, or nullptr on allocation
 *         failure.  If @p s is nullptr the behaviour is undefined.
 */
char* csilk_arena_strdup(csilk_arena_t* arena, const char* s);

/**
 * @brief Duplicate @p n bytes of a string using the arena allocator.
 *
 * @param arena  The arena allocator.
 * @param s      Source string to duplicate.
 * @param n      Number of bytes to copy.
 * @return A copy of @p n bytes of @p s allocated from @p arena, or nullptr on
 *         allocation failure.  If @p s is nullptr the behaviour is undefined.
 */
char* csilk_arena_strndup(csilk_arena_t* arena, const char* s, size_t n);

/**
 * @brief Free all memory chunks owned by the arena.
 *
 * After this call the arena pointer is invalid and must not be used again.
 *
 * @param arena  The arena allocator to destroy.
 */
void csilk_arena_free(csilk_arena_t* arena);

/**
 * @brief Reset the arena without freeing its chunks.
 *
 * The arena can be reused after a reset — subsequent allocations reuse
 * the existing chunk memory.  Useful for pooling arenas across requests
 * to reduce malloc pressure.
 *
 * @param arena  The arena allocator to reset.
 */
void csilk_arena_reset(csilk_arena_t* arena);

/**
 * @brief Enable or disable 64-byte alignment for an arena.
 *
 * When enabled, all subsequent allocations from this arena will be aligned
 * to a 64-byte boundary (cache line).  This is useful for preventing false
 * sharing in high-concurrency environments but increases memory overhead.
 *
 * @param arena   The arena allocator.
 * @param enabled Non-zero to enable 64-byte alignment, 0 for default 8-byte.
 */
void csilk_arena_set_alignment(csilk_arena_t* arena, int enabled);

void csilk_arena_get_stats(csilk_arena_t* arena, size_t* total_size, size_t* total_used);

#endif /* CSILK_SERVER_H */
