/**
 * @file db.c
 * @brief Unified database interface implementation.
 *
 * ## Architecture
 * The DB layer uses a pluggable driver registry pattern:
 *
 *   csilk_db_driver_t (abstract interface)
 *     ├── connect(pool, dsn) → establishes connection
 *     ├── query(pool, sql, result) → returns tabular results
 *     ├── exec(pool, sql) → executes non-query statements
 *     ├── free_result(result) → releases query results
 *     └── disconnect(pool) → tears down connection
 *
 * Drivers self-register at startup (e.g., csilk_db_sqlite_init() calls
 * csilk_db_register_driver()). The registry is a simple fixed-size array
 * protected by a mutex.
 *
 * ## Pool lifecycle
 * csilk_db_pool_new() → driver->connect() → [query/exec] → pool_free() →
 * driver->disconnect()
 *
 * The pool itself is a thin wrapper: it holds a driver pointer, a mutex
 * (for serializing access to the single connection), and driver-specific
 * state in an opaque `handle` field.
 *
 * ## JSON result conversion
 * csilk_db_query_json() and friends convert the driver's tabular result
 * (csilk_db_result_t) into a csilk_json array of objects — one object per row,
 * with column names as keys. This is the format expected by the HTTP layer
 * for JSON API responses.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/db.h"
#include "db_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/core/json/json.h"

/* --- Global DB Metrics --- */
static atomic_uint_fast64_t db_queries_total = 0;
static atomic_uint_fast64_t db_execs_total = 0;
static atomic_uint_fast64_t db_errors_total = 0;
static atomic_uint_fast64_t db_duration_us_total = 0;

/** @brief Read aggregated database metrics atomically.
 * @param stats [out] Pointer to a csilk_db_stats_t struct to populate.
 * @note Safe to call from any thread. */
void
csilk_db_get_stats(csilk_db_stats_t* stats)
{
    if (!stats) {
        return;
    }
    stats->queries_total = atomic_load(&db_queries_total);
    stats->execs_total = atomic_load(&db_execs_total);
    stats->errors_total = atomic_load(&db_errors_total);
    stats->duration_us_total = atomic_load(&db_duration_us_total);
}

/** @brief Get the raw driver-level connection handle.
 * @return The connection pointer, or NULL if pool is NULL.
 * @deprecated Use csilk_db_with_conn() for safe cross-thread access, or
 *             csilk_db_pool_acquire()/csilk_db_pool_put() for explicit
 *             acquire/release pairs.  The bare getter returns a raw pointer
 *             with no holder tracking, no lifetime constraint, and no
 *             mutex acquisition — cross-thread use of the returned pointer
 *             is undefined behaviour and was the root cause of the
 *             heap-use-after-free reported in csilk#1 (Minefolio dual-thread
 *             model driving the same SQLite/PG connection from two threads).
 *
 * @note The caller must hold the pool mutex if concurrent access is possible.
 * @note For new code, prefer csilk_db_with_conn(pool, fn, ctx) which holds
 *       pool->mutex for the duration of fn and performs holder tracking,
 *       or csilk_db_pool_acquire() + csilk_db_pool_put() for explicit
 *       RAII-style acquire/release pairs.
 */
void*
csilk_db_pool_get_connection(csilk_db_pool_t* pool)
{
    return pool ? pool->connection : NULL;
}

/** @brief Explicitly acquire the pool's connection with holder tracking.
 *
 * Atomically (under pool->mutex) records the calling thread as the holder of
 * pool->connection.  Must be paired with csilk_db_pool_put() to release.
 * The pair forms an RAII-style contract: every successful acquire() must be
 * followed by exactly one put(), even on the SQL-failure path.
 *
 * @param pool     Connection pool.  Must not be NULL.
 * @param conn     [out] Receives the raw driver-level connection pointer.
 *                 Valid until the matching csilk_db_pool_put() call.
 * @return 0 on success, -1 if pool is NULL or the connection is not open.
 *
 * @note Intended for downstream code that needs to drive the driver API
 *       (sqlite3_exec / PQexec / etc.) directly.  For pure high-level use,
 *       csilk_db_with_conn() is preferred because it hides the raw pointer
 *       and the acquire/put pairing.
 */
int
csilk_db_pool_acquire(csilk_db_pool_t* pool, void** conn)
{
    if (!pool || !conn) {
        CSILK_LOG_E("csilk_db_pool_acquire: invalid pool or conn out-param");
        return -1;
    }

    csilk_mutex_lock(&pool->mutex);
    if (!pool->connection) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("csilk_db_pool_acquire: pool '%s' has no open connection",
                    pool->driver ? pool->driver->name : "(null)");
        return -1;
    }
    /* Reject if the connection is already held by another thread.  This is
     * what makes the holder check in csilk_db_pool_put meaningful: without
     * this guard, a second thread could simply re-acquire and overwrite
     * conn_holder, defeating the cross-thread release refusal. */
    if (pool->conn_holder_set) {
        csilk_thread_t me = csilk_thread_self();
        if (me != pool->conn_holder) {
            CSILK_LOG_W("csilk_db_pool_acquire: pool '%s' already held by another "
                        "thread; refusing concurrent acquire",
                        pool->driver ? pool->driver->name : "(null)");
            csilk_mutex_unlock(&pool->mutex);
            return -1;
        }
        /* Same thread re-acquiring — idempotent, just return the conn. */
        *conn = pool->connection;
        csilk_mutex_unlock(&pool->mutex);
        return 0;
    }
    pool->conn_holder = csilk_thread_self();
    pool->conn_holder_set = true;
    *conn = pool->connection;
    csilk_mutex_unlock(&pool->mutex);
    return 0;
}

/** @brief Release a previously-acquired connection back to the pool.
 *
 * Performs a holder check under pool->mutex: only the thread that called
 * csilk_db_pool_acquire() (or the one currently recorded in conn_holder)
 * may release the connection.  A release by any other thread is refused
 * with -1 and a CSILK_LOG_W, to prevent the "release someone else's conn"
 * bug that allowed two threads to share a freed connection (csilk#1).
 *
 * The release itself is a single atomic write to pool->conn_holder_set
 * (while holding the mutex), so it cannot race with a concurrent
 * csilk_db_pool_acquire() or with the high-level csilk_db_query_* /
 * csilk_db_exec_* paths which take pool->mutex for their whole duration.
 *
 * @param pool     Connection pool.  Must not be NULL.
 * @param conn     The pointer returned by the matching csilk_db_pool_acquire().
 *                 Must equal pool->connection; a mismatch returns -1.
 * @return 0 on success, -1 on any validation failure.
 */
int
csilk_db_pool_put(csilk_db_pool_t* pool, void* conn)
{
    if (!pool || !conn) {
        CSILK_LOG_E("csilk_db_pool_put: invalid pool or conn");
        return -1;
    }

    csilk_mutex_lock(&pool->mutex);

    /* The connection must still be the one we handed out. */
    if (pool->connection != conn) {
        CSILK_LOG_W("csilk_db_pool_put: conn %p does not match pool->connection %p "
                    "(driver '%s'); refusing to release",
                    conn,
                    pool->connection,
                    pool->driver ? pool->driver->name : "(null)");
        csilk_mutex_unlock(&pool->mutex);
        return -1;
    }

    /* Holder check: only the acquiring thread may release. */
    if (pool->conn_holder_set) {
        csilk_thread_t me = csilk_thread_self();
        if (me != pool->conn_holder) {
            CSILK_LOG_W("csilk_db_pool_put: called from thread that does not hold "
                        "the connection (acquired by another thread); refusing "
                        "cross-thread release (driver '%s')",
                        pool->driver ? pool->driver->name : "(null)");
            csilk_mutex_unlock(&pool->mutex);
            return -1;
        }
    }
    pool->conn_holder_set = false;
    /* Note: pool->connection is NOT cleared here — the connection remains
     * open and owned by the pool until csilk_db_pool_free(). */
    csilk_mutex_unlock(&pool->mutex);
    return 0;
}

/** @brief Callback signature for csilk_db_with_conn().
 *
 * @param conn     The raw driver-level connection handle.
 * @param user_ctx Caller-supplied context forwarded verbatim.
 * @return 0 on success, or any driver-specific error code (e.g. -1) on
 *         failure.  The return value is propagated to the caller.
 */
typedef int (*csilk_db_with_conn_fn)(void* conn, void* user_ctx);

/** @brief Run a callback under the pool mutex with holder tracking.
 *
 * This is the recommended safe access pattern for cross-thread database use.
 * The callback runs while pool->mutex is held and conn_holder is set to the
 * calling thread, so:
 *
 *   - No other thread can concurrently drive the same connection.
 *   - The caller never sees a raw pointer; the connection is returned to the
 *     pool automatically on callback exit (success or failure).
 *   - No "forgot to put on the failure path" bug is possible.
 *
 * @param pool     Connection pool.  Must not be NULL.
 * @param fn       Callback to invoke.  Receives the raw connection pointer
 *                 and user_ctx.
 * @param user_ctx Opaque user data forwarded to fn.
 * @return The return value of fn, or -1 on invalid arguments / no connection.
 *
 * @example
 * @code
 * int my_query(void* conn, void* ctx) {
 *     sqlite_conn_t* s = (sqlite_conn_t*)conn;
 *     // drive sqlite3_exec(s->db, "SELECT ...") directly
 *     return 0;
 * }
 * csilk_db_with_conn(pool, my_query, my_ctx);
 * @endcode
 */
int
csilk_db_with_conn(csilk_db_pool_t* pool, csilk_db_with_conn_fn fn, void* user_ctx)
{
    if (!pool || !fn) {
        CSILK_LOG_E("csilk_db_with_conn: invalid pool or fn");
        return -1;
    }

    csilk_mutex_lock(&pool->mutex);
    if (!pool->connection) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("csilk_db_with_conn: pool '%s' has no open connection",
                    pool->driver ? pool->driver->name : "(null)");
        return -1;
    }
    /* Set holder so a concurrent csilk_db_pool_put from another thread is
     * refused by the holder check. */
    pool->conn_holder = csilk_thread_self();
    pool->conn_holder_set = true;

    int rc = fn(pool->connection, user_ctx);

    pool->conn_holder_set = false;
    csilk_mutex_unlock(&pool->mutex);
    return rc;
}

const char*
csilk_db_pool_get_driver_name(csilk_db_pool_t* pool)
{
    return (pool && pool->driver) ? pool->driver->name : NULL;
}

/** @brief Store a driver-level connection handle.
 *
 * Typically called by the driver's connect() callback.  The pool takes
 * ownership of the pointer; the driver's disconnect() callback is
 * responsible for freeing it. */
void
csilk_db_pool_set_connection(csilk_db_pool_t* pool, void* conn)
{
    if (pool) {
        pool->connection = conn;
    }
}

/** @brief Internal: execute a query and return the result as a csilk_json array.
 *
 * ## Row-to-JSON conversion
 *   1. Call driver->query() to get the raw tabular result (rows + columns).
 *   2. Create an empty csilk_json array.
 *   3. For each row (result.rows[i]):
 *      a. Create a csilk_json object.
 *      b. For each field (row->values[j]):
 *         - Add (column_name[j], field_value) as a string key-value pair.
 *         - If column_names is NULL, use "col" as the key.
 *         - NULL values are skipped (not added to the object).
 *      c. Append the object to the array.
 *   4. Call driver->free_result() to release the driver's memory.
 *   5. Return the csilk_json array (caller must csilk_json_free()).
 *
 * All values are represented as csilk_json strings — no type inference is
 * attempted. The caller can parse numerics/bools with csilk_json_get_number
 * etc. if needed.
 *
 * @param pool Database pool with an active connection.
 * @param sql  SQL query string.
 * @return A csilk_json array of row objects, or NULL on failure.
 * @note The returned csilk_json must be freed by the caller with csilk_json_free().
 * @warning The pool mutex must be held for the duration of this call. */
static csilk_json_t*
csilk_db_query_json_locked(csilk_db_pool_t* pool, const char* sql)
{
    csilk_db_result_t result = {0};
    if (pool->driver->query(pool, sql, &result) != 0) {
        return NULL;
    }

    csilk_json_t* array = csilk_json_array();
    if (!array) {
        pool->driver->free_result(&result);
        return NULL;
    }

    for (int i = 0; i < result.row_count; i++) {
        csilk_json_t* obj = csilk_json_object();
        if (!obj) {
            csilk_json_free(array);
            pool->driver->free_result(&result);
            return NULL;
        }

        csilk_db_row_t* row = result.rows[i];
        for (int j = 0; j < row->count; j++) {
            if (row->values[j]) {
                csilk_json_add_object(obj,
                                      result.column_names ? result.column_names[j] : "col",
                                      csilk_json_string_new(row->values[j]));
            }
        }
        csilk_json_array_append(array, obj);
    }

    pool->driver->free_result(&result);
    return array;
}

/** @brief Create a new database connection pool with the named driver.
 *
 * ## Pool creation
 *   1. Look up the driver by name in the global registry.
 *   2. calloc the pool struct, init the pool mutex.
 *   3. Call driver->connect() with the DSN — this may block for network
 *      round-trips.
 *   4. On success: return the pool. On failure: destroy mutex, free pool.
 *
 * The pool holds exactly ONE connection (mutex-serialized access). This is
 * intentional — the caller is expected to manage a pool of pools if
 * concurrency is needed.
 *
 * @param driver_name Registered driver name (e.g., "sqlite3").
 * @param dsn         Data source name (e.g., "/tmp/test.db" or "host=..."). */
csilk_db_pool_t*
csilk_db_pool_new(const char* driver_name, const char* dsn)
{
    if (!driver_name) {
        CSILK_LOG_E("Failed to create database pool: driver_name is NULL");
        return NULL;
    }

    csilk_db_driver_t* driver = csilk_db_get_driver(driver_name);
    if (!driver) {
        CSILK_LOG_E("Failed to create database pool: driver '%s' not found", driver_name);
        return NULL;
    }

    csilk_db_pool_t* pool = calloc(1, sizeof(csilk_db_pool_t));
    if (!pool) {
        CSILK_LOG_E("Failed to allocate memory for database pool (driver: '%s')", driver_name);
        return NULL;
    }

    csilk_mutex_init(&pool->mutex);
    pool->driver = driver;
    pool->conn_holder_set = false;
    pool->conn_holder = csilk_thread_self(); /* placeholder; overwritten on acquire */
    if (driver->connect(pool, dsn) != 0) {
        CSILK_LOG_E("Database connection failed for driver '%s' (DSN: '%s')", driver_name, dsn);
        csilk_mutex_destroy(&pool->mutex);
        free(pool);
        return NULL;
    }

    CSILK_LOG_I(
        "Database pool created and connected using driver '%s' (DSN: '%s')", driver_name, dsn);
    return pool;
}

/** @brief Free a database pool and its underlying connection.
 *
 * ## Teardown
 *   1. Call driver->disconnect() — closes the database connection.
 *   2. Destroy the pool mutex.
 *   3. Free the pool struct.
 *
 * The driver's free_result is not called here — any outstanding results
 * must have been freed by the caller. */
void
csilk_db_pool_free(csilk_db_pool_t* pool)
{
    if (!pool) {
        return;
    }
    CSILK_LOG_D("Freeing database pool and closing connection for driver '%s'",
                pool->driver ? pool->driver->name : "unknown");
    if (pool->driver && pool->driver->disconnect) {
        pool->driver->disconnect(pool);
    }
    csilk_mutex_destroy(&pool->mutex);
    free(pool);
}

/*
 * Record the most recent error category on the pool (csilk#8).
 *
 * If the driver provides a last_error callback, consult it; otherwise
 * fall back to the generic core mapping.  The generic mapping is coarse
 * but still more useful than a bare -1: a NULL connection means ERR_CONN,
 * a driver that refused the SQL means ERR_SQL.
 */
static void
_csilk_db_record_error(csilk_db_pool_t* pool, bool had_connection)
{
    if (!pool) {
        return;
    }
    if (!had_connection) {
        /* No connection — the failure is a connection-level problem. */
        pool->last_err = CSILK_DB_ERR_CONN;
        return;
    }
    /* Consult the driver's last_error callback when present. */
    if (pool->driver && pool->driver->last_error) {
        csilk_db_err_t err = CSILK_DB_ERR_SQL;
        if (pool->driver->last_error(pool, &err) == 0) {
            pool->last_err = err;
            return;
        }
    }
    /* Generic fallback. */
    pool->last_err = CSILK_DB_ERR_SQL;
}

/** @brief Register an on-open hook (csilk#4). */
void
csilk_db_pool_on_open(csilk_db_pool_t* pool, csilk_db_on_open_fn cb, void* ctx)
{
    if (!pool) {
        return;
    }
    csilk_mutex_lock(&pool->mutex);
    pool->on_open_cb = cb;
    pool->on_open_ctx = ctx;
    /* If a connection is already open, apply the hook now so the pool
     * benefits immediately (idempotent). */
    void* conn = pool->connection;
    csilk_mutex_unlock(&pool->mutex);
    if (cb && conn) {
        cb(pool, ctx);
    }
}

/** @brief Get the most recent database error category (csilk#8). */
csilk_db_err_t
csilk_db_last_error(csilk_db_pool_t* pool)
{
    if (!pool) {
        return CSILK_DB_ERR_CONN;
    }
    return pool->last_err;
}

/**
 * @brief Apply a dialect-specific database optimisation (csilk#4).
 *
 * Delegates to the driver's set_pragma callback when present; otherwise
 * renders the kind to a SQLite PRAGMA string and executes it (SQLite is
 * the only dialect csilk has a built-in driver for that exposes PRAGMA).
 * A driver that does not support the kind returns 0 (no-op) so the same
 * caller code works regardless of the active driver.
 */
int
csilk_db_set_pragma(csilk_db_pool_t* pool, int kind, const char* value)
{
    if (!pool || !pool->driver) {
        CSILK_LOG_E("csilk_db_set_pragma: invalid pool or driver");
        return -1;
    }

    /* Prefer the driver's own rendering when it provides one. */
    if (pool->driver->set_pragma) {
        return pool->driver->set_pragma(pool, kind, value);
    }

    /* Fallback: render a SQLite PRAGMA string and execute it.  This is
     * correct when the driver is SQLite; for non-SQLite drivers the
     * PRAGMA string will fail, which the caller should treat as a no-op
     * (the no-driver-callback path is only reached by drivers that did
     * not implement set_pragma — currently the built-in SQLite driver,
     * which does implement it, so this fallback is rarely taken). */
    char sql[256];
    switch (kind) {
    case CSILK_DB_PRAGMA_JOURNAL_MODE:
        snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s;", value ? value : "WAL");
        break;
    case CSILK_DB_PRAGMA_BUSY_TIMEOUT:
        snprintf(sql, sizeof(sql), "PRAGMA busy_timeout=%s;", value ? value : "5000");
        break;
    case CSILK_DB_PRAGMA_SYNCHRONOUS:
        snprintf(sql, sizeof(sql), "PRAGMA synchronous=%s;", value ? value : "NORMAL");
        break;
    case CSILK_DB_PRAGMA_FOREIGN_KEYS:
        snprintf(sql, sizeof(sql), "PRAGMA foreign_keys=%s;", value ? value : "ON");
        break;
    case CSILK_DB_PRAGMA_CUSTOM:
    default:
        snprintf(sql, sizeof(sql), "%s", value ? value : "");
        break;
    }
    if (!sql[0]) {
        return 0; /* nothing to do */
    }
    CSILK_LOG_W("csilk_db_set_pragma: driver '%s' has no set_pragma callback; "
                "rendering SQLite PRAGMA fallback: %s",
                pool->driver->name ? pool->driver->name : "unknown",
                sql);
    int rc = csilk_db_exec(pool, sql);
    return rc;
}

/** @brief Execute a SQL query and return the result as a csilk_json array. */
csilk_json_t*
csilk_db_query_json(csilk_db_pool_t* pool, const char* sql)
{
    if (!pool || !pool->driver || !pool->driver->query) {
        CSILK_LOG_E("Database query failed: invalid pool, driver, or query method");
        return NULL;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Database query initiated: %s", sql);

    csilk_mutex_lock(&pool->mutex);
    void*         conn_snapshot = pool->connection;
    csilk_json_t* result = csilk_db_query_json_locked(pool, sql);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (!result) {
        atomic_fetch_add(&db_errors_total, 1);
        _csilk_db_record_error(pool, conn_snapshot != NULL);
        CSILK_LOG_E(
            "Database query failed (duration: %.2f ms): %s", (double)duration / 1000.0, sql);
    } else {
        pool->last_err = CSILK_DB_OK;
        atomic_fetch_add(&db_queries_total, 1);
        CSILK_LOG_T(
            "Database query succeeded (duration: %.2f ms): %s", (double)duration / 1000.0, sql);
    }
    return result;
}

typedef struct {
    csilk_io_work_t   req;
    csilk_db_pool_t*  pool;
    char*             sql;
    csilk_db_async_cb cb;
    void*             user_data;
    csilk_json_t*     result;
} db_async_work_ctx_t;

/** @brief Thread-pool work callback — runs the query off the main loop.
 * Stores the resulting JSON in the async context for the after-work callback. */
static void
db_async_work_cb(csilk_io_work_t* req)
{
    db_async_work_ctx_t* ctx = (db_async_work_ctx_t*)req;
    ctx->result = csilk_db_query_json(ctx->pool, ctx->sql);
}

/** @brief After-work callback — invokes the user callback on the main loop
 * thread, then frees the SQL string and the async context. */
static void
db_async_after_work_cb(csilk_io_work_t* req, int status)
{
    (void)status;
    db_async_work_ctx_t* ctx = (db_async_work_ctx_t*)req;
    if (ctx->cb) {
        ctx->cb(ctx->result, ctx->user_data);
    }
    free(ctx->sql);
    free(ctx);
}

/**
 * @brief Execute a SQL query asynchronously and deliver the JSON result.
 *
 * Allocates an async context, queues csilk_db_query_json() on the I/O thread
 * pool, and returns immediately. The caller's callback fires on the main loop
 * thread once the query completes; ownership of the resulting csilk_json
 * (when non-NULL) passes to the callback.
 *
 * @param pool      Database pool (must not be NULL).
 * @param sql       SQL query string (must not be NULL).
 * @param cb        Completion callback (must not be NULL).
 * @param user_data Opaque value forwarded to the callback.
 * @return 0 if the work was queued, -1 on invalid arguments or OOM. */
int
csilk_db_query_json_async(csilk_db_pool_t*  pool,
                          const char*       sql,
                          csilk_db_async_cb cb,
                          void*             user_data)
{
    if (!pool || !sql || !cb) {
        return -1;
    }
    db_async_work_ctx_t* ctx = calloc(1, sizeof(db_async_work_ctx_t));
    if (!ctx) {
        return -1;
    }
    ctx->pool = pool;
    ctx->sql = strdup(sql);
    ctx->cb = cb;
    ctx->user_data = user_data;

    csilk_io_loop_t* loop = csilk_io_default_loop();
    int rc = csilk_io_queue_work(loop, &ctx->req, db_async_work_cb, db_async_after_work_cb);
    if (rc != 0) {
        free(ctx->sql);
        free(ctx);
        return -1;
    }
    return 0;
}

/** @brief Execute a SQL statement (INSERT, UPDATE, DELETE, DDL) that does not
 * return rows. */
int
csilk_db_exec(csilk_db_pool_t* pool, const char* sql)
{
    if (!pool || !pool->driver || !pool->driver->exec) {
        CSILK_LOG_E("Database exec failed: invalid pool, driver, or exec method");
        return -1;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Database exec initiated: %s", sql);

    csilk_mutex_lock(&pool->mutex);
    void* conn_snapshot = pool->connection;
    int   rc = pool->driver->exec(pool, sql);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (rc != 0) {
        atomic_fetch_add(&db_errors_total, 1);
        _csilk_db_record_error(pool, conn_snapshot != NULL);
        CSILK_LOG_E("Database exec failed with rc %d (duration: %.2f ms): %s",
                    rc,
                    (double)duration / 1000.0,
                    sql);
    } else {
        pool->last_err = CSILK_DB_OK;
        atomic_fetch_add(&db_execs_total, 1);
        CSILK_LOG_T(
            "Database exec succeeded (duration: %.2f ms): %s", (double)duration / 1000.0, sql);
    }
    return rc;
}

/** @brief Execute a parameterized SQL query and return JSON result.
 *
 * ## String substitution algorithm
 *   1. Pre-compute the final SQL length: original SQL + sum of all param
 *      lengths + 2 bytes per param for surrounding single quotes.
 *   2. Allocate a buffer of that size.
 *   3. Walk the original SQL character by character:
 *      - '?' → replace with 'value' (single-quote wrapped)
 *      - any other char → copy verbatim.
 *   4. Execute the constructed SQL via csilk_db_query_json_locked().
 *
 * ## Security
 * Parameter values are escaped for SQL injection: single quotes and
 * backslashes are doubled before string substitution.  This is NOT an
 * ORM or prepared-statement binding — it trades a small performance
 * cost for safety over the previous naive substitution.
 *
 * @param pool   Database pool.
 * @param sql    SQL pattern with ? placeholders.
 * @param params NULL-terminated array of string values. */
/*
 * Auto-detect the SQL type of a string parameter value.
 *
 *   - pure integer (optional leading '-', digits) → INT64
 *   - pure float/double (digits [.digits] [eE[+-]digits]) → DOUBLE
 *   - anything else → TEXT
 *
 * The detection is conservative: any character outside the numeric grammar
 * (spaces, 'n', 'x', etc.) forces TEXT, so values like "0001" are INT64
 * (value 1) and "1.5e3" is DOUBLE.  This matches SQLite's column-type
 * affinity expectations: numeric columns get native numerics, TEXT
 * columns keep their literal.
 *
 * @return The csilk_db_param_kind_t to bind this value with.
 */
static csilk_db_param_kind_t
_csilk_db_auto_detect_param_kind(const char* v)
{
    if (!v || !v[0]) {
        return CSILK_DB_PARAM_TEXT; /* empty string → TEXT (not NULL; NULL is explicit) */
    }

    /* INTEGER: optional sign + digits. */
    const char* p = v;
    if (*p == '-' || *p == '+') {
        p++;
    }
    if (*p && (unsigned char)*p >= '0' && (unsigned char)*p <= '9') {
        const char* q = p;
        while (*q && (unsigned char)*q >= '0' && (unsigned char)*q <= '9') {
            q++;
        }
        if (*q == '\0') {
            return CSILK_DB_PARAM_INT64;
        }
    }

    /* DOUBLE: use strtod; require the whole string to be consumed and the
     * value to look like a float (contains '.', 'e', 'E', or is a numeric
     * literal strtod would parse).  We reject pure-integer-looking strings
     * that already returned INT64 above. */
    char*  end = NULL;
    double d = strtod(v, &end);
    if (end && end != v && *end == '\0') {
        /* strtod consumed the whole string.  If the string had no '.' or
         * 'e'/'E', it was already an INT64 above; reaching here means it
         * was a float literal like "1.5", "2e3", ".5", "1e-2". */
        const char* s = v;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '-' || *s == '+') {
            s++;
        }
        bool has_float_part = false;
        for (const char* r = s; *r; r++) {
            if (*r == '.' || *r == 'e' || *r == 'E') {
                has_float_part = true;
                break;
            }
        }
        if (has_float_part) {
            return CSILK_DB_PARAM_DOUBLE;
        }
    }

    return CSILK_DB_PARAM_TEXT;
}

/*
 * Bind a single parameter value (auto-detected from a string) onto a
 * prepared statement.  Used by the legacy string-based API so the caller
 * keeps the same const char** signature but gets real typed binding.
 */
static int
_csilk_db_bind_auto_param(csilk_stmt_t* stmt, int index, const char* val)
{
    if (!val) {
        return csilk_stmt_bind_null(stmt, index);
    }
    switch (_csilk_db_auto_detect_param_kind(val)) {
    case CSILK_DB_PARAM_INT64:
        return csilk_stmt_bind_int64(stmt, index, (int64_t)strtoll(val, NULL, 10));
    case CSILK_DB_PARAM_DOUBLE:
        return csilk_stmt_bind_double(stmt, index, strtod(val, NULL));
    case CSILK_DB_PARAM_NULL:
        return csilk_stmt_bind_null(stmt, index);
    case CSILK_DB_PARAM_TEXT:
    default:
        return csilk_stmt_bind_text(stmt, index, val);
    }
}

/*
 * Bind an explicit typed parameter onto a prepared statement.
 */
static int
_csilk_db_bind_typed_param(csilk_stmt_t* stmt, int index, const csilk_db_param_t* p)
{
    switch (p->kind) {
    case CSILK_DB_PARAM_INT64:
        return csilk_stmt_bind_int64(stmt, index, p->int64_val);
    case CSILK_DB_PARAM_DOUBLE:
        return csilk_stmt_bind_double(stmt, index, p->double_val);
    case CSILK_DB_PARAM_NULL:
        return csilk_stmt_bind_null(stmt, index);
    case CSILK_DB_PARAM_TEXT:
        return csilk_stmt_bind_text(stmt, index, p->text_val ? p->text_val : "");
    default:
        return -1; /* out-of-range sentinel */
    }
}

/*
 * Run a prepared statement with auto-typed string parameters and return the
 * result as a type-preserving JSON array.  Shared by csilk_db_query_param_json
 * and csilk_db_query_param_json_typed.
 */
static csilk_json_t*
_csilk_db_run_param_stmt_query(csilk_db_pool_t* pool, const char* sql, const char** params)
{
    if (!pool || !sql) {
        return NULL;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Parameterized database query initiated (prepared): %s", sql);

    csilk_mutex_lock(&pool->mutex);
    csilk_stmt_t* stmt = NULL;
    int           rc = pool->driver->stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("Parameterized query failed to prepare statement: %s", sql);
        atomic_fetch_add(&db_errors_total, 1);
        return NULL;
    }

    /* Bind each string param with auto-detection. */
    int bind_ok = 0;
    for (int i = 0; params[i]; i++) {
        if (_csilk_db_bind_auto_param(stmt, i + 1, params[i]) != 0) {
            break;
        }
        bind_ok = 1;
    }

    csilk_json_t* result = NULL;
    if (bind_ok) {
        result = pool->driver->stmt_query_json(stmt);
    }
    pool->driver->stmt_close(stmt);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (!result) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E("Parameterized database query failed (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    sql);
    } else {
        atomic_fetch_add(&db_queries_total, 1);
        CSILK_LOG_T("Parameterized database query succeeded (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    sql);
    }
    return result;
}

/*
 * Run a prepared statement with explicit typed parameters and return the
 * result as a type-preserving JSON array.
 */
static csilk_json_t*
_csilk_db_run_param_stmt_query_typed(csilk_db_pool_t*        pool,
                                     const char*             sql,
                                     const csilk_db_param_t* params)
{
    if (!pool || !sql || !params) {
        return NULL;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Parameterized database query initiated (typed prepared): %s", sql);

    csilk_mutex_lock(&pool->mutex);
    csilk_stmt_t* stmt = NULL;
    int           rc = pool->driver->stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("Typed parameterized query failed to prepare statement: %s", sql);
        atomic_fetch_add(&db_errors_total, 1);
        return NULL;
    }

    /* Bind each typed param; stop at the sentinel (out-of-range kind).
     * all_binds_ok stays true if we reached the sentinel without any bind
     * failure; it is false if a bind failed before the sentinel.  An
     * empty parameter list (sentinel at index 0) leaves all_binds_ok true
     * and binds nothing — a 0-param SELECT/DML is valid and still runs. */
    bool all_binds_ok = true;
    for (int i = 0;; i++) {
        if (params[i].kind > CSILK_DB_PARAM_NULL) {
            break; /* sentinel reached; every prior bind succeeded */
        }
        if (_csilk_db_bind_typed_param(stmt, i + 1, &params[i]) != 0) {
            all_binds_ok = false;
            break;
        }
    }

    csilk_json_t* result = NULL;
    if (all_binds_ok) {
        result = pool->driver->stmt_query_json(stmt);
    }
    pool->driver->stmt_close(stmt);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (!result) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E("Typed parameterized database query failed (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    sql);
    } else {
        atomic_fetch_add(&db_queries_total, 1);
    }
    return result;
}

/*
 * Run a prepared statement with auto-typed string parameters (DML path).
 */
static int
_csilk_db_run_param_stmt_exec(csilk_db_pool_t* pool, const char* sql, const char** params)
{
    if (!pool || !sql || !params) {
        return -1;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Parameterized database exec initiated (prepared): %s", sql);

    csilk_mutex_lock(&pool->mutex);
    csilk_stmt_t* stmt = NULL;
    int           rc = pool->driver->stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("Parameterized exec failed to prepare statement: %s", sql);
        return -1;
    }

    int bind_ok = 1;
    for (int i = 0; params[i]; i++) {
        if (_csilk_db_bind_auto_param(stmt, i + 1, params[i]) != 0) {
            bind_ok = 0;
            break;
        }
    }

    int64_t aff = 0;
    int     exec_rc = -1;
    if (bind_ok) {
        exec_rc = pool->driver->stmt_execute(stmt, &aff);
    }
    pool->driver->stmt_close(stmt);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (exec_rc != 0) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E(
            "Parameterized exec failed (duration: %.2f ms): %s", (double)duration / 1000.0, sql);
    } else {
        atomic_fetch_add(&db_execs_total, 1);
    }
    return exec_rc;
}

/*
 * Run a prepared statement with explicit typed parameters (DML path).
 */
static int
_csilk_db_run_param_stmt_exec_typed(csilk_db_pool_t*        pool,
                                    const char*             sql,
                                    const csilk_db_param_t* params)
{
    if (!pool || !sql || !params) {
        return -1;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Parameterized database exec initiated (typed prepared): %s", sql);

    csilk_mutex_lock(&pool->mutex);
    csilk_stmt_t* stmt = NULL;
    int           rc = pool->driver->stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        csilk_mutex_unlock(&pool->mutex);
        CSILK_LOG_E("Typed parameterized exec failed to prepare statement: %s", sql);
        return -1;
    }

    int bind_ok = 1;
    for (int i = 0;; i++) {
        if (params[i].kind > CSILK_DB_PARAM_NULL) {
            break; /* sentinel */
        }
        if (_csilk_db_bind_typed_param(stmt, i + 1, &params[i]) != 0) {
            bind_ok = 0;
            break;
        }
    }

    int64_t aff = 0;
    int     exec_rc = -1;
    if (bind_ok) {
        exec_rc = pool->driver->stmt_execute(stmt, &aff);
    }
    pool->driver->stmt_close(stmt);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (exec_rc != 0) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E("Typed parameterized exec failed (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    sql);
    } else {
        atomic_fetch_add(&db_execs_total, 1);
    }
    return exec_rc;
}

/** @brief Execute a parameterised SELECT query with ? placeholders.
 *
 * Now backed by a real prepared statement (csilk#5): parameters are
 * auto-typed and bound via the driver's stmt_bind_* callbacks instead of
 * string concatenation.  The result set is type-preserving (csilk#2).
 *
 * @param pool   Connection pool.
 * @param sql    SQL pattern with ? placeholders.
 * @param params NULL-terminated array of string values. */
csilk_json_t*
csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params)
{
    if (!pool || !sql || !params) {
        CSILK_LOG_E("Parameterized query failed: invalid pool, sql, or params");
        return NULL;
    }
    return _csilk_db_run_param_stmt_query(pool, sql, params);
}

csilk_json_t*
csilk_db_query_param_json_typed(csilk_db_pool_t*        pool,
                                const char*             sql,
                                const csilk_db_param_t* params)
{
    if (!pool || !sql || !params) {
        CSILK_LOG_E("Typed parameterized query failed: invalid arguments");
        return NULL;
    }
    return _csilk_db_run_param_stmt_query_typed(pool, sql, params);
}

/** @brief Execute a parameterised statement (INSERT/UPDATE/DELETE) with ?
 *  placeholders.
 *
 * Now backed by a real prepared statement (csilk#5): parameters are
 * auto-typed and bound via the driver's stmt_bind_* callbacks instead of
 * string concatenation, which eliminates both the type-loss (numeric
 * columns compared as strings) and the SQL-injection escape (incomplete
 * quote-doubling in LIKE / comment contexts) of the old path.
 *
 * @param pool   Database pool.
 * @param sql    SQL with ? placeholders.
 * @param params NULL-terminated array of string values.
 * @return 0 on success, -1 on failure.
 *
 * @note For full control over parameter types, use csilk_db_exec_param_typed(). */
int
csilk_db_exec_param(csilk_db_pool_t* pool, const char* sql, const char** params)
{
    if (!pool || !sql || !params) {
        CSILK_LOG_E("Parameterized exec failed: invalid arguments");
        return -1;
    }
    return _csilk_db_run_param_stmt_exec(pool, sql, params);
}

int
csilk_db_exec_param_typed(csilk_db_pool_t* pool, const char* sql, const csilk_db_param_t* params)
{
    if (!pool || !sql || !params) {
        CSILK_LOG_E("Typed parameterized exec failed: invalid arguments");
        return -1;
    }
    return _csilk_db_run_param_stmt_exec_typed(pool, sql, params);
}

/* --- Driver Registry --- */

/** @brief Statically-sized registry of registered database drivers (max 16). */
static csilk_db_driver_t* drivers[16];
static int                driver_count = 0;
static csilk_mutex_t      registry_mutex;
static int                registry_initialized = 0;

/** @brief Lazily initialise the driver-registry mutex (idempotent). */
static void
ensure_registry_init(void)
{
    if (!registry_initialized) {
        csilk_mutex_init(&registry_mutex);
        registry_initialized = 1;
    }
}

/** @brief Initialise the database subsystem.
 *
 * Registers all built-in drivers (SQLite3, MySQL, PostgreSQL, etc.).
 * Must be called once before any csilk_db_pool_new call.
 * Safe to call multiple times. */
void
csilk_db_init(void)
{
    ensure_registry_init();
    csilk_db_sqlite_init();
#ifdef HAS_MYSQL
    csilk_db_mysql_init();
#endif
#ifdef HAS_POSTGRES
    csilk_db_postgres_init();
#endif
#ifdef HAS_MONGODB
    csilk_db_mongodb_init();
#endif
#ifdef HAS_REDIS
    csilk_db_redis_init();
#endif
}

/** @brief Register a database driver in the global registry.
 *
 * The driver's vtable (with connect/query/exec/disconnect callbacks) must
 * remain valid for the process lifetime.  Up to 16 drivers can be registered.
 *
 * @param name   Unique driver name for later lookup.
 * @param driver Heap-allocated driver vtable (not copied, only the pointer is
 *               stored). Must outlive the registry.
 * @return 0 on success, -1 if name/driver is NULL or registry is full. */
int
csilk_db_register_driver(const char* name, csilk_db_driver_t* driver)
{
    if (!name || !driver) {
        CSILK_LOG_E("Failed to register database driver: name or driver is NULL");
        return -1;
    }
    ensure_registry_init();

    csilk_mutex_lock(&registry_mutex);
    for (int i = 0; i < driver_count; i++) {
        if (strcmp(drivers[i]->name, name) == 0) {
            drivers[i] = driver;
            CSILK_LOG_I("Overwrote database driver: '%s'", name);
            csilk_mutex_unlock(&registry_mutex);
            return 0;
        }
    }
    if (driver_count >= 16) {
        CSILK_LOG_E("Failed to register database driver '%s': registry is full", name);
        csilk_mutex_unlock(&registry_mutex);
        return -1;
    }
    drivers[driver_count++] = driver;
    CSILK_LOG_I("Registered database driver: '%s'", name);
    csilk_mutex_unlock(&registry_mutex);
    return 0;
}

/** @brief Look up a registered database driver by name.
 *
 * Linear search of the driver registry (at most 16 entries, so O(1) in
 * practice).  Must call csilk_db_init() first to register built-in drivers.
 *
 * @param name Driver name (case-sensitive, e.g. "sqlite3").
 * @return Driver pointer, or NULL if not found or @p name is NULL. */
csilk_db_driver_t*
csilk_db_get_driver(const char* name)
{
    if (!name) {
        return NULL;
    }
    ensure_registry_init();

    csilk_mutex_lock(&registry_mutex);
    for (int i = 0; i < driver_count; i++) {
        if (strcmp(drivers[i]->name, name) == 0) {
            csilk_mutex_unlock(&registry_mutex);
            return drivers[i];
        }
    }
    csilk_mutex_unlock(&registry_mutex);
    return NULL;
}

int
csilk_stmt_prepare(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt)
{
    if (!pool || !sql || !out_stmt) {
        return -1;
    }
    if (!pool->driver || !pool->driver->stmt_prepare) {
        return -1;
    }
    csilk_mutex_lock(&pool->mutex);
    int rc = pool->driver->stmt_prepare(pool, sql, out_stmt);
    csilk_mutex_unlock(&pool->mutex);
    return rc;
}

int
csilk_stmt_bind_int64(csilk_stmt_t* stmt, int index, int64_t val)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_bind_int64) {
        return -1;
    }
    return stmt->pool->driver->stmt_bind_int64(stmt, index, val);
}

int
csilk_stmt_bind_double(csilk_stmt_t* stmt, int index, double val)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_bind_double) {
        return -1;
    }
    return stmt->pool->driver->stmt_bind_double(stmt, index, val);
}

int
csilk_stmt_bind_text(csilk_stmt_t* stmt, int index, const char* val)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_bind_text) {
        return -1;
    }
    return stmt->pool->driver->stmt_bind_text(stmt, index, val);
}

int
csilk_stmt_bind_null(csilk_stmt_t* stmt, int index)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_bind_null) {
        return -1;
    }
    return stmt->pool->driver->stmt_bind_null(stmt, index);
}

int
csilk_stmt_execute(csilk_stmt_t* stmt, int64_t* out_affected_rows)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_execute) {
        return -1;
    }
    csilk_mutex_lock(&stmt->pool->mutex);
    int rc = stmt->pool->driver->stmt_execute(stmt, out_affected_rows);
    csilk_mutex_unlock(&stmt->pool->mutex);
    return rc;
}

csilk_json_t*
csilk_stmt_query_json(csilk_stmt_t* stmt)
{
    if (!stmt || !stmt->pool || !stmt->pool->driver || !stmt->pool->driver->stmt_query_json) {
        return NULL;
    }
    csilk_mutex_lock(&stmt->pool->mutex);
    csilk_json_t* res = stmt->pool->driver->stmt_query_json(stmt);
    csilk_mutex_unlock(&stmt->pool->mutex);
    return res;
}

void
csilk_stmt_close(csilk_stmt_t* stmt)
{
    if (!stmt) {
        return;
    }
    csilk_db_pool_t* pool = stmt->pool;
    if (pool && pool->driver && pool->driver->stmt_close) {
        csilk_mutex_lock(&pool->mutex);
        pool->driver->stmt_close(stmt);
        csilk_mutex_unlock(&pool->mutex);
    } else {
        free(stmt);
    }
}
