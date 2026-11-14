#pragma once
/**
 * @file csilk_db.h
 * @brief Unified pluggable database interface for the csilk web framework.
 *
 * Provides an abstract driver model (similar to Go's database/sql) so that
 * different backends (SQLite, PostgreSQL, MySQL, etc.) can be used through a
 * common API.  All public database functions are declared here; driver
 * implementations live in separate compilation units.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/json/json.h"

/* Forward declarations — avoids pulling the entire public API via csilk.h.
   types.h defines csilk_storage_driver_t which this header needs. */
#include "csilk/core/types.h"

/**
 * @brief Database statistics.
 */
typedef struct {
    uint64_t queries_total;     /**< Total SELECT queries executed. */
    uint64_t execs_total;       /**< Total non-query statements (INSERT/UPDATE/etc). */
    uint64_t errors_total;      /**< Total failed operations. */
    uint64_t duration_us_total; /**< Cumulative duration in microseconds. */
} csilk_db_stats_t;

/**
 * @brief Get current database statistics.
 * @param stats [out] Pointer to stats struct to populate.
 */
void csilk_db_get_stats(csilk_db_stats_t* stats);

/**
 * @brief Opaque database driver interface.
 *
 * Each supported database backend implements the function table in
 * csilk_db_driver_s and registers itself with csilk_db_register_driver.
 */
typedef struct csilk_db_driver_s csilk_db_driver_t;

/** @brief Opaque database connection pool handle. */
typedef struct csilk_db_pool_s csilk_db_pool_t;

/** @brief Opaque prepared statement handle. */
typedef struct csilk_stmt_s csilk_stmt_t;

/**
 * @brief Distinguishable database error categories.
 *
 * Returned by csilk_db_last_error() after a failed csilk_db_exec /
 * csilk_db_query_json / csilk_db_exec_param call.  Before this enumeration
 * (csilk#8), every failure path returned the same -1, so callers could not
 * tell "SQL syntax error" from "connection dropped" from "the driver does
 * not support this SQL dialect" from "lock timeout" from "OOM" — downstream
 * was forced to maintain its own driver-detection static flag (e.g.
 * Minefolio's g_is_postgres) to branch on dialect.
 *
 * @note The category is set by the driver (via the csilk_db_driver_t
 *       last_error callback when present) or falls back to the core layer's
 *       generic mapping.  A value of CSILK_DB_OK means the most recent
 *       operation succeeded (no error recorded).
 */
typedef enum {
    CSILK_DB_OK = 0,      /**< Most recent operation succeeded. */
    CSILK_DB_ERR_SQL,     /**< SQL syntax / runtime error. */
    CSILK_DB_ERR_CONN,    /**< Connection dropped / could not connect. */
    CSILK_DB_ERR_DIALECT, /**< The driver does not support this SQL dialect. */
    CSILK_DB_ERR_BUSY,    /**< Lock timeout / database is busy. */
    CSILK_DB_ERR_OOM      /**< Out of memory. */
} csilk_db_err_t;

/**
 * @brief Dialect-specific database optimisation kinds.
 *
 * Used by csilk_db_set_pragma() to apply a driver-appropriate optimisation
 * (SQLite PRAGMA / PostgreSQL SET).  The csilk core renders each kind to
 * the correct dialect via the driver's set_pragma callback, so callers no
 * longer need to branch on driver name (csilk#4).
 */
typedef enum {
    CSILK_DB_PRAGMA_JOURNAL_MODE = 1, /**< value: "WAL" / "DELETE" / ... */
    CSILK_DB_PRAGMA_BUSY_TIMEOUT,     /**< value: milliseconds, e.g. "5000" */
    CSILK_DB_PRAGMA_SYNCHRONOUS,      /**< value: "NORMAL" / "FULL" / ... */
    CSILK_DB_PRAGMA_FOREIGN_KEYS,     /**< value: "ON" / "OFF" */
    CSILK_DB_PRAGMA_CUSTOM            /**< value: raw dialect SQL string */
} csilk_db_pragma_kind_t;

/**
 * @brief Parameter value types for typed binding (csilk_db_query_param_json_typed).
 *
 * Use these to bind parameters with their native SQL types instead of
 * letting the driver infer from a string.  `CSILK_DB_PARAM_NULL` binds
 * SQL NULL; the int64/double/TEXT variants bind the corresponding native
 * types, eliminating the type-loss and SQL-injection concerns of the
 * legacy string-substitution path (csilk#5).
 */
typedef enum {
    CSILK_DB_PARAM_INT64,
    CSILK_DB_PARAM_DOUBLE,
    CSILK_DB_PARAM_TEXT,
    CSILK_DB_PARAM_NULL
} csilk_db_param_kind_t;

/**
 * @brief A typed SQL parameter binding.
 *
 * Passed to csilk_db_query_param_json_typed / csilk_db_exec_param_typed.
 * The array is NULL-terminated (a sentinel with kind == CSILK_DB_PARAM_NULL
 * and a zero/NULL value is not sufficient — terminate with a {0} entry whose
 * kind is a value greater than CSILK_DB_PARAM_NULL, or simply end the array
 * with a fully-zeroed struct; the binding loop stops at the first all-zero
 * struct).  For convenience, the helper csilk_db_params_end() returns a
 * sentinel you can append.
 */
typedef struct {
    csilk_db_param_kind_t kind;
    int64_t               int64_val;
    double                double_val;
    const char* text_val; /* NUL-terminated; valid only when kind == CSILK_DB_PARAM_TEXT */
} csilk_db_param_t;

/** @brief Sentinel value to terminate a csilk_db_param_t array. */
static inline csilk_db_param_t
csilk_db_params_end(void)
{
    csilk_db_param_t p;
    p.kind = (csilk_db_param_kind_t)(CSILK_DB_PARAM_NULL + 1); /* out-of-range sentinel */
    p.int64_val = 0;
    p.double_val = 0.0;
    p.text_val = NULL;
    return p;
}

/** @brief Get the driver-specific connection handle from a pool.
 * @param pool Database pool.
 * @return Opaque connection pointer (driver-specific type).
 * @deprecated Use csilk_db_with_conn() for cross-thread safe access, or the
 *             csilk_db_pool_acquire()/csilk_db_pool_put() pair.  The bare
 *             getter returns a raw pointer with no holder tracking, no
 *             lifetime constraint, and no mutex acquisition; cross-thread
 *             use of the returned pointer is undefined behaviour and caused
 *             the heap-use-after-free reported in csilk#1.
 */
void* csilk_db_pool_get_connection(csilk_db_pool_t* pool);

/**
 * @brief Explicitly acquire the pool's connection with holder tracking.
 *
 * Records the calling thread as the holder of pool->connection.  Must be
 * paired with csilk_db_pool_put() to release — every successful acquire()
 * requires exactly one put(), even on the SQL-failure path.  Forms the
 * RAII-style "take-and-return" contract that eliminates the UAF class of
 * bugs reported in csilk#1.
 *
 * @param pool  Connection pool.  Must not be NULL.
 * @param conn  [out] Receives the raw driver-level connection pointer.
 * @return 0 on success, -1 if pool is NULL or the connection is not open.
 */
int csilk_db_pool_acquire(csilk_db_pool_t* pool, void** conn);

/**
 * @brief Release a previously-acquired connection back to the pool.
 *
 * Performs a holder check: only the thread that called csilk_db_pool_acquire()
 * may release the connection.  A release by any other thread is refused with
 * -1 and a CSILK_LOG_W, to prevent the "release someone else's conn" bug
 * that allowed two threads to share a freed connection (csilk#1).
 *
 * @param pool  Connection pool.  Must not be NULL.
 * @param conn  The pointer returned by the matching acquire().
 * @return 0 on success, -1 on validation failure.
 */
int csilk_db_pool_put(csilk_db_pool_t* pool, void* conn);

/** @brief Callback signature for csilk_db_with_conn(). */
typedef int (*csilk_db_with_conn_fn)(void* conn, void* user_ctx);

/**
 * @brief Run a callback under the pool mutex with holder tracking.
 *
 * The recommended safe access pattern for cross-thread database use.  The
 * callback runs while pool->mutex is held and conn_holder is set to the
 * calling thread, so no other thread can concurrently drive the same
 * connection, and the caller never sees a raw pointer — the connection is
 * returned automatically on callback exit (success or failure).  No
 * "forgot to put on the failure path" bug is possible.
 *
 * @param pool      Connection pool.  Must not be NULL.
 * @param fn        Callback to invoke; receives the raw connection pointer
 *                 and user_ctx.
 * @param user_ctx  Opaque user data forwarded to fn.
 * @return The return value of fn, or -1 on invalid arguments / no connection.
 */
int csilk_db_with_conn(csilk_db_pool_t* pool, csilk_db_with_conn_fn fn, void* user_ctx);

/**
 * @brief Get the driver name of a database pool (e.g., "sqlite", "postgres").
 * @param pool Database pool.
 * @return Driver name, or NULL if pool is NULL or driver is unknown.
 */
const char* csilk_db_pool_get_driver_name(csilk_db_pool_t* pool);

/**
 * @brief Set the driver-specific connection handle on a pool.
 * @param pool Database pool.
 * @param conn Opaque connection pointer.
 */
void csilk_db_pool_set_connection(csilk_db_pool_t* pool, void* conn);

/**
 * @brief A single row of a query result.
 *
 * Values are strings (human-readable representations of column data).
 * NULL database values are represented as NULL pointers in the values array.
 */
typedef struct {
    char** values; /**< Array of NUL-terminated string values, one per column.
                    NULL if the SQL value was NULL. */
    int    count;  /**< Number of columns (length of @p values). */
} csilk_db_row_t;

/**
 * @brief A complete query result set.
 *
 * Contains all rows, column names, and dimension information.
 * Allocated by the driver's query function and freed by free_result.
 */
typedef struct {
    csilk_db_row_t** rows;         /**< Array of row pointers (length @p row_count). */
    char**           column_names; /**< Array of column name strings (length @p
                            column_count). */
    int              row_count;    /**< Number of rows in @p rows. */
    int              column_count; /**< Number of columns (length of @p column_names and each
                       row's @p values). */
} csilk_db_result_t;

/**
 * @brief Virtual function table implemented by each database driver.
 *
 * All function pointers must be non-NULL except where noted.
 */
struct csilk_db_driver_s {
    const char* name; /**< Driver identifier string (e.g., "sqlite", "postgres").
                       Matches the name passed to csilk_db_pool_new. */

    /** @brief Open a connection to the database.
   *  @param pool  Pool whose @p connection field should be populated.
   *  @param dsn   Driver-specific data source name.
   *  @return 0 on success, -1 on failure. */
    int (*connect)(csilk_db_pool_t* pool, const char* dsn);

    /** @brief Close the database connection.
   *  @param pool  Pool whose @p connection should be closed and freed.
   *  @return 0 on success, -1 on failure. */
    int (*disconnect)(csilk_db_pool_t* pool);

    /** @brief Execute a SELECT query and produce a result set.
   *  @param pool   Connection pool (mutex is held by the caller).
   *  @param sql    SQL query string.
   *  @param[out] result  Caller-allocated csilk_db_result_t to populate.
   *                      Set to zero-initialised (the driver allocates rows +
   *                      strings internally).
   *  @return 0 on success, -1 on failure. */
    int (*query)(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result);

    /** @brief Execute a statement that returns no result rows.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @param sql   SQL statement (INSERT, UPDATE, DELETE, DDL, etc.).
   *  @return 0 on success, -1 on failure. */
    int (*exec)(csilk_db_pool_t* pool, const char* sql);

    /** @brief Begin a database transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
    int (*transaction_begin)(csilk_db_pool_t* pool);

    /** @brief Commit the current transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
    int (*transaction_commit)(csilk_db_pool_t* pool);

    /** @brief Roll back the current transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
    int (*transaction_rollback)(csilk_db_pool_t* pool);

    /** @brief Check if the connection is currently inside an active transaction.
   *  @param pool  Connection pool.
   *  @return 1 if in transaction, 0 if not, -1 on error. */
    int (*in_transaction)(csilk_db_pool_t* pool);

    /** @brief Prepared statement hooks */
    int (*stmt_prepare)(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt);
    int (*stmt_bind_int64)(csilk_stmt_t* stmt, int index, int64_t val);
    int (*stmt_bind_double)(csilk_stmt_t* stmt, int index, double val);
    int (*stmt_bind_text)(csilk_stmt_t* stmt, int index, const char* val);
    int (*stmt_bind_null)(csilk_stmt_t* stmt, int index);
    int (*stmt_execute)(csilk_stmt_t* stmt, int64_t* out_affected_rows);
    csilk_json_t* (*stmt_query_json)(csilk_stmt_t* stmt);
    void (*stmt_close)(csilk_stmt_t* stmt);

    /** @brief Free a result set and all associated memory.
   *  @param result  Result set to free (must not be NULL). */
    void (*free_result)(csilk_db_result_t* result);

    /** @brief Map the most recent failure to a csilk_db_err_t category.
   *  Optional (NULL for drivers that do not implement it — the core
   *  layer falls back to a generic mapping).  Called by
   *  csilk_db_last_error() after an exec/query failure to obtain a
   *  driver-specific error category (e.g. SQLite maps SQLITE_BUSY to
   *  CSILK_DB_ERR_BUSY; PostgreSQL maps a disconnected PQstatus to
   *  CSILK_DB_ERR_CONN).
   *  @param pool  Connection pool.
   *  @param[out] out_err  Receives the category.
   *  @return 0 on success, -1 if the driver could not determine a
   *          category (in which case the core layer uses its fallback). */
    int (*last_error)(csilk_db_pool_t* pool, csilk_db_err_t* out_err);

    /** @brief Apply a dialect-specific pragma / SET optimisation.
   *  Optional (NULL for drivers that do not support dialect tweaks).
   *  Invoked by csilk_db_set_pragma() to render the appropriate dialect
   *  string (SQLite PRAGMA / PostgreSQL SET).  @p kind is a
   *  csilk_db_pragma_kind_t; @p value is the dialect-specific payload
   *  (e.g. "WAL" for journal_mode, 5000 for busy_timeout).
   *  @return 0 on success or no-op, -1 on unsupported dialect. */
    int (*set_pragma)(csilk_db_pool_t* pool, int kind, const char* value);
};

/**
 * @brief Transaction and savepoint scope handle.
 */
typedef struct {
    char name[64];
    bool is_savepoint;
    bool active;
} csilk_db_tx_scope_t;

/**
 * @brief Check if the connection pool is currently inside an active transaction.
 *
 * @param pool Connection pool.
 * @return 1 if in transaction, 0 if not, -1 on error.
 */
int csilk_db_in_transaction(csilk_db_pool_t* pool);

/**
 * @brief Begin a transaction or savepoint scope.
 *
 * If the connection is not yet in a transaction, starts a top-level transaction.
 * If already in a transaction, creates a nested SAVEPOINT with a unique name.
 *
 * @param pool  Connection pool.
 * @param name  Base identifier name for the scope or savepoint.
 * @param scope Scope struct to populate.
 * @return 0 on success, -1 on failure.
 */
int csilk_db_tx_scope_begin(csilk_db_pool_t* pool, const char* name, csilk_db_tx_scope_t* scope);

/**
 * @brief Commit a transaction or release a savepoint scope.
 *
 * @param pool  Connection pool.
 * @param scope Active scope struct.
 * @return 0 on success, -1 on failure.
 */
int csilk_db_tx_scope_commit(csilk_db_pool_t* pool, csilk_db_tx_scope_t* scope);

/**
 * @brief Roll back a transaction or roll back to a savepoint scope.
 *
 * @param pool  Connection pool.
 * @param scope Active scope struct.
 * @return 0 on success, -1 on failure.
 */
int csilk_db_tx_scope_rollback(csilk_db_pool_t* pool, csilk_db_tx_scope_t* scope);

/**
 * @brief Prepare a SQL statement on the database pool.
 *
 * @param pool     Connection pool.
 * @param sql      SQL query/statement with ? parameter placeholders.
 * @param out_stmt Pointer to receive prepared statement handle.
 * @return 0 on success, -1 on failure.
 */
int csilk_stmt_prepare(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt);

/**
 * @brief Bind a 64-bit integer to a statement parameter (1-based index).
 */
int csilk_stmt_bind_int64(csilk_stmt_t* stmt, int index, int64_t val);

/**
 * @brief Bind a double float to a statement parameter (1-based index).
 */
int csilk_stmt_bind_double(csilk_stmt_t* stmt, int index, double val);

/**
 * @brief Bind a text string to a statement parameter (1-based index).
 */
int csilk_stmt_bind_text(csilk_stmt_t* stmt, int index, const char* val);

/**
 * @brief Bind SQL NULL to a statement parameter (1-based index).
 */
int csilk_stmt_bind_null(csilk_stmt_t* stmt, int index);

/**
 * @brief Execute a non-query prepared statement (INSERT, UPDATE, DELETE).
 *
 * @param stmt              Prepared statement handle.
 * @param out_affected_rows Optional pointer to receive number of affected rows.
 * @return 0 on success, -1 on failure.
 */
int csilk_stmt_execute(csilk_stmt_t* stmt, int64_t* out_affected_rows);

/**
 * @brief Execute a query prepared statement and return result rows as a JSON array.
 *
 * Preserves column types natively (numbers as numbers, text as text, NULL as null).
 *
 * @param stmt Prepared statement handle.
 * @return JSON array of row objects, or NULL on failure. Caller frees.
 */
csilk_json_t* csilk_stmt_query_json(csilk_stmt_t* stmt);

/**
 * @brief Finalize and close a prepared statement.
 *
 * @param stmt Prepared statement handle to free. Safe if NULL.
 */
void csilk_stmt_close(csilk_stmt_t* stmt);

/**
 * @brief Create a new database pool and connect using the named driver.
 *
 * Looks up the driver by @p driver_name (must have been registered via
 * csilk_db_register_driver or the built-in init), allocates a pool, and
 * calls the driver's connect function.
 *
 * @param driver_name  Driver identifier (e.g., "sqlite").  Must not be NULL.
 * @param dsn          Data source name (driver-specific format).
 * @return A new csilk_db_pool_t on success, or NULL if the driver is unknown
 *         or the connection fails.
 */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/**
 * @brief Create a multi-connection database pool (csilk#6).
 *
 * Opens @p max_conns connections to the driver and stores them in an idle
 * list.  csilk_db_pool_acquire picks a free connection; csilk_db_pool_put
 * returns it.  For single-connection pools (max_conns == 1) the behaviour
 * is identical to csilk_db_pool_new.
 *
 * High-level APIs (csilk_db_query_json, csilk_db_exec, etc.) always use
 * connections[0] under the pool mutex.  The multi-connection value is that
 * concurrent callers of csilk_db_pool_acquire can each get a DIFFERENT
 * connection and work in parallel (PostgreSQL / MySQL / Redis).  For SQLite
 * the file-level lock serialises writes, so max_conns > 1 is mostly useful
 * for concurrent read-only access.
 *
 * @param driver_name  Driver identifier (e.g., "postgres").  Must not be NULL.
 * @param dsn          Data source name (driver-specific format).
 * @param max_conns    Number of connections to open (1–32).  Clamped to range.
 * @return A new csilk_db_pool_t on success, or NULL on failure.
 */
csilk_db_pool_t* csilk_db_pool_new_ex(const char* driver_name, const char* dsn, int max_conns);

/**
 * @brief Register an on-open hook that runs after the driver's connect()
 *        succeeds.
 *
 * The callback is invoked with the pool and the caller-supplied context
 * right after the connection is established, letting the downstream apply
 * dialect optimisations (PRAGMA / SET) at connect time rather than in a
 * separate db_init step (csilk#4).  The hook runs under pool->mutex.
 *
 * @param pool   Connection pool.
 * @param cb     Callback (may be NULL to clear the hook).
 * @param ctx    Opaque user data forwarded to cb.
 */
typedef void (*csilk_db_on_open_fn)(csilk_db_pool_t* pool, void* ctx);
void csilk_db_pool_on_open(csilk_db_pool_t* pool, csilk_db_on_open_fn cb, void* ctx);

/**
 * @brief Get the most recent database error category.
 *
 * Returns the csilk_db_err_t category recorded by the last failed
 * csilk_db_exec / csilk_db_query_json / csilk_db_exec_param / typed
 * variant call.  Returns CSILK_DB_OK if the most recent call succeeded
 * (no error recorded).  The category is obtained from the driver's
 * last_error callback when present, otherwise from the core layer's
 * generic mapping (csilk#8).
 *
 * @param pool  Connection pool.  Must not be NULL.
 * @return The most recent csilk_db_err_t category.
 */
csilk_db_err_t csilk_db_last_error(csilk_db_pool_t* pool);

/**
 * @brief Apply a dialect-specific database optimisation.
 *
 * Renders @p kind to the appropriate dialect string for the pool's driver
 * (SQLite PRAGMA / PostgreSQL SET) and executes it.  This lets callers
 * express an optimisation intent (e.g. "enable WAL + busy_timeout")
 * without hand-writing driver-specific SQL (csilk#4).
 *
 * For drivers that do not support a given kind (e.g. SQLite PRAGMA on
 * PostgreSQL, or PG SET on SQLite), the call is a no-op that logs a
 * CSILK_LOG_W and returns 0 — NOT an error, so callers can safely issue
 * the same set of optimisations regardless of the active driver.
 *
 * @param pool   Connection pool.
 * @param kind   Optimisation kind (csilk_db_pragma_kind_t).
 * @param value  Dialect-specific payload (e.g. "WAL", "5000", "ON").
 *               For CSILK_DB_PRAGMA_CUSTOM, the raw dialect SQL string.
 * @return 0 on success or no-op (driver does not support the kind),
 *         -1 on a genuine failure (SQL error, OOM, etc.).
 */
int csilk_db_set_pragma(csilk_db_pool_t* pool, int kind, const char* value);

/**
 * @brief Destroy a database pool and disconnect.
 *
 * Calls the driver's disconnect, frees internal resources, and deallocates
 * the pool struct.
 *
 * @param pool  Pool to destroy.  Must not be NULL.
 */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/**
 * @brief Execute a SELECT query and return the rows as a JSON array.
 *
 * Internally acquires the pool mutex, calls the driver's query function,
 * converts each row to a JSON object (keyed by column name), frees the
 * native result, and returns the csilk_json array.
 *
 * @param pool  Connection pool.
 * @param sql   SQL SELECT statement.
 * @return A csilk_json array of row objects, or NULL on failure.  Caller must
 *         free with csilk_json_free.
 */
csilk_json_t* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/** @brief Callback type for async database query execution. */
typedef void (*csilk_db_async_cb)(csilk_json_t* result, void* user_data);

/** @brief Callback type for async database non-query (DML/DDL) execution. */
typedef void (*csilk_db_exec_async_cb)(int rc, void* user_data);

/**
 * @brief Execute a SQL query asynchronously without blocking the event loop.
 *
 * Offloads the database query to the I/O thread pool and invokes @p cb on the
 * event loop thread when complete.
 *
 * @param pool      Connection pool.
 * @param sql       SQL query string.
 * @param cb        Callback to invoke with the query result (csilk_json_t*).
 * @param user_data Opaque user data pointer.
 * @return 0 if offloaded successfully, -1 on failure.
 */
int csilk_db_query_json_async(csilk_db_pool_t*  pool,
                              const char*       sql,
                              csilk_db_async_cb cb,
                              void*             user_data);

/**
 * @brief Execute a SQL statement (INSERT/UPDATE/DELETE/DDL) asynchronously.
 *
 * Offloads csilk_db_exec() to the I/O thread pool and invokes @p cb on the
 * event loop thread with the return code (0 on success, -1 on error).
 *
 * @param pool      Connection pool.
 * @param sql       SQL statement string.
 * @param cb        Callback to invoke upon completion.
 * @param user_data Opaque user data pointer.
 * @return 0 if offloaded successfully, -1 on failure.
 */
int csilk_db_exec_async(csilk_db_pool_t*       pool,
                        const char*            sql,
                        csilk_db_exec_async_cb cb,
                        void*                  user_data);

/**
 * @brief Execute a parameterised SELECT query asynchronously.
 *
 * Deep-copies @p sql and the NULL-terminated @p params array, runs
 * csilk_db_query_param_json() on the I/O thread pool, and delivers the
 * type-preserving JSON array on the event loop thread.
 *
 * @param pool      Connection pool.
 * @param sql       SQL query with ? placeholders.
 * @param params    NULL-terminated array of string values (may be NULL if no params).
 * @param cb        Callback to invoke with the query result (csilk_json_t*).
 * @param user_data Opaque user data pointer.
 * @return 0 if offloaded successfully, -1 on failure.
 */
int csilk_db_query_param_json_async(csilk_db_pool_t*  pool,
                                    const char*       sql,
                                    const char**      params,
                                    csilk_db_async_cb cb,
                                    void*             user_data);

/**
 * @brief Execute a parameterised DML statement asynchronously.
 *
 * Deep-copies @p sql and the NULL-terminated @p params array, runs
 * csilk_db_exec_param() on the I/O thread pool, and invokes @p cb on the
 * event loop thread with the return code (0 on success, -1 on error).
 *
 * @param pool      Connection pool.
 * @param sql       SQL statement with ? placeholders.
 * @param params    NULL-terminated array of string values (may be NULL if no params).
 * @param cb        Callback to invoke upon completion.
 * @param user_data Opaque user data pointer.
 * @return 0 if offloaded successfully, -1 on failure.
 */
int csilk_db_exec_param_async(csilk_db_pool_t*       pool,
                              const char*            sql,
                              const char**           params,
                              csilk_db_exec_async_cb cb,
                              void*                  user_data);

/**
 * @brief Execute a statement that produces no result rows.
 *
 * Acquires the pool mutex and calls the driver's exec function.
 *
 * @param pool  Connection pool.
 * @param sql   SQL statement (INSERT, UPDATE, DELETE, DDL).
 * @return 0 on success, -1 on failure.
 */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders.
 *
 * Each ? in @p sql is bound to the corresponding value from @p params via a
 * real prepared statement (sqlite3_prepare_v2 + sqlite3_bind_* on SQLite;
 * PQprepare + PQexecPrepared on PostgreSQL).  Parameter values are
 * auto-typed: a pure integer is bound as INT64, a pure float/double as
 * DOUBLE, anything else as TEXT.  This eliminates the type-loss and
 * SQL-injection concerns of the previous string-substitution path
 * (csilk#5) and returns a type-preserving JSON result (csilk#2).
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params NULL-terminated array of string values.  The number of
 *               values must match the number of ? placeholders.
 * @return A csilk_json array of row objects, or NULL on failure.  Caller must
 *         free with csilk_json_free.
 *
 * @note For full control over parameter types (e.g. binding a specific
 *       int64 instead of auto-detecting from a string), use
 *       csilk_db_query_param_json_typed().
 */
csilk_json_t*
csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders, with
 *        explicit type annotations.
 *
 * Like csilk_db_query_param_json but accepts a csilk_db_param_t array so
 * the caller controls each parameter's SQL type (INT64 / DOUBLE / TEXT /
 * NULL) instead of relying on auto-detection.  The array must be
 * terminated by csilk_db_params_end().
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params Sentinel-terminated csilk_db_param_t array.
 * @return A type-preserving csilk_json array of row objects, or NULL on failure.
 */
csilk_json_t* csilk_db_query_param_json_typed(csilk_db_pool_t*        pool,
                                              const char*             sql,
                                              const csilk_db_param_t* params);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders and opt-in
 *        JSON column decoding.
 *
 * Like csilk_db_query_param_json, but additionally accepts a NULL-terminated
 * array of column names (@p json_columns) that should be JSON-decoded.  For
 * each declared column whose value is a string that looks like JSON (starts
 * with '[' or '{' and parses successfully), the value is replaced by the
 * parsed JSON object/array; otherwise the value is kept as a string scalar.
 * Columns not in @p json_columns keep the default type-preserving behaviour.
 *
 * @param pool         Connection pool.
 * @param sql          SQL with ? placeholders.
 * @param params       NULL-terminated array of string values.
 * @param json_columns NULL-terminated array of column names to JSON-decode
 *                     (csilk#2, option B).
 * @return A type-preserving csilk_json array of row objects, or NULL on failure.
 *         Caller must free with csilk_json_free.
 */
csilk_json_t* csilk_db_query_param_json_with_columns(csilk_db_pool_t*   pool,
                                                     const char*        sql,
                                                     const char**       params,
                                                     const char* const* json_columns);

/**
 * @brief Execute a SELECT query and return the rows as a JSON array with
 *        opt-in JSON column decoding.
 *
 * Like csilk_db_query_json, but additionally accepts a NULL-terminated array
 * of column names (@p json_columns) that should be JSON-decoded.  For each
 * declared column whose value is a string that looks like JSON (starts with
 * '[' or '{' and parses successfully), the value is replaced by the parsed
 * JSON object/array; otherwise the value is kept as a string scalar.
 * Columns not in @p json_columns keep the default all-string behaviour.
 *
 * @param pool         Connection pool.
 * @param sql          SQL SELECT statement.
 * @param json_columns NULL-terminated array of column names to JSON-decode
 *                     (csilk#2, option B).
 * @return A csilk_json array of row objects, or NULL on failure.  Caller must
 *         free with csilk_json_free.
 */
csilk_json_t* csilk_db_query_json_with_columns(csilk_db_pool_t*   pool,
                                               const char*        sql,
                                               const char* const* json_columns);

/**
 * @brief Execute a parameterised statement (INSERT/UPDATE/DELETE) with ?
 *        placeholders.
 *
 * Each ? in @p sql is bound to the corresponding value from @p params via a
 * real prepared statement (auto-typed: INT64 / DOUBLE / TEXT).  This
 * prevents SQL injection in DML statements without the type-loss of the
 * previous string-substitution path (csilk#5).
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params NULL-terminated array of string values.  The number of
 *               values must match the number of ? placeholders.
 * @return 0 on success, -1 on failure.
 *
 * @note For full control over parameter types, use csilk_db_exec_param_typed().
 */
int csilk_db_exec_param(csilk_db_pool_t* pool, const char* sql, const char** params);

/**
 * @brief Execute a parameterised statement (INSERT/UPDATE/DELETE) with ?
 *        placeholders, with explicit type annotations.
 *
 * Like csilk_db_exec_param but accepts a csilk_db_param_t array so the
 * caller controls each parameter's SQL type.  The array must be terminated
 * by csilk_db_params_end().
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params Sentinel-terminated csilk_db_param_t array.
 * @return 0 on success, -1 on failure.
 */
int
csilk_db_exec_param_typed(csilk_db_pool_t* pool, const char* sql, const csilk_db_param_t* params);

/**
 * @brief Register a database driver implementation.
 *
 * Makes the driver available for use with csilk_db_pool_new.  The @p name
 * must be unique (earlier registrations are not overwritten).
 *
 * @param name    Driver identifier string (e.g., "postgres").  Must remain
 *                valid for the program lifetime.
 * @param driver  Pointer to a csilk_db_driver_t with all function pointers
 *                populated.  The struct must remain valid for the program
 *                lifetime.
 * @return 0 on success, -1 if a driver with @p name is already registered.
 */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver);

/**
 * @brief Look up a registered driver by name.
 *
 * @param name  Driver identifier string.
 * @return Pointer to the registered csilk_db_driver_t, or NULL if not found.
 */
csilk_db_driver_t* csilk_db_get_driver(const char* name);

/**
 * @brief Internal: Register the built-in SQLite3 driver.
 *
 * Called automatically by csilk_db_init.  Not intended for direct use.
 */
void csilk_db_sqlite_init(void);

/**
 * @brief Internal: Register the built-in MySQL driver.
 *
 * Called automatically by csilk_db_init.  Requires libmysqlclient.
 * Not intended for direct use.
 */
#ifdef HAS_MYSQL
void csilk_db_mysql_init(void);
#endif

/**
 * @brief Internal: Register the built-in PostgreSQL driver.
 *
 * Called automatically by csilk_db_init.  Requires libpq.
 * Not intended for direct use.
 */
#ifdef HAS_POSTGRES
void csilk_db_postgres_init(void);
#endif

/**
 * @brief Internal: Register the built-in MongoDB driver.
 *
 * Called automatically by csilk_db_init. Requires libmongoc.
 * Not intended for direct use.
 */
#ifdef HAS_MONGODB
void csilk_db_mongodb_init(void);
#endif

/**
 * @brief Internal: Register the built-in Redis driver.
 *
 * Called automatically by csilk_db_init. Requires hiredis.
 * Not intended for direct use.
 */
#ifdef HAS_REDIS
void csilk_db_redis_init(void);

/**
 * @brief Create a storage driver backed by a Redis connection pool.
 *
 * This allows using Redis as the backend for context storage, distributed
 * session management, and rate limiting via the csilk_storage_driver_t interface.
 *
 * @param pool  The Redis database pool (must be initialized and connected).
 * @return A heap-allocated storage driver instance, or NULL on failure.
 *         The caller is responsible for freeing the driver when shutting down.
 */
csilk_storage_driver_t* csilk_redis_storage_driver_new(csilk_db_pool_t* pool);
#endif

/**
 * @brief Initialise the database subsystem.
 *
 * Registers all built-in drivers (SQLite3, MySQL, PostgreSQL, MongoDB, Redis).
 * Must be called once before any csilk_db_pool_new call.
 * Safe to call multiple times.
 */
void csilk_db_init(void);
