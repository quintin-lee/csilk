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

#include "csilk/core/json.h"

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
 * @note The caller must hold the pool mutex if concurrent access is possible. */
void*
csilk_db_pool_get_connection(csilk_db_pool_t* pool)
{
    return pool ? pool->connection : NULL;
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
    csilk_json_t* result = csilk_db_query_json_locked(pool, sql);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (!result) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E(
            "Database query failed (duration: %.2f ms): %s", (double)duration / 1000.0, sql);
    } else {
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
    int rc = pool->driver->exec(pool, sql);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (rc != 0) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E("Database exec failed with rc %d (duration: %.2f ms): %s",
                    rc,
                    (double)duration / 1000.0,
                    sql);
    } else {
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
csilk_json_t*
csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params)
{
    if (!pool || !sql || !params) {
        CSILK_LOG_E("Parameterized query failed: invalid pool, sql, or params");
        return NULL;
    }

    uint64_t start = csilk_io_hrtime();
    CSILK_LOG_D("Parameterized database query initiated: %s", sql);

    /* Pre-scan: compute output size with SQL-escaped param values.
     * Each ? is replaced with a single-quoted, escaped value. */
    size_t len = strlen(sql);
    for (int i = 0; params[i]; i++) {
        len += 2; /* surrounding single quotes */
        for (const char* c = params[i]; *c; c++) {
            len += (*c == '\'') ? 2 : 1;
        }
    }

    char* full_sql = malloc(len + 1);
    if (!full_sql) {
        CSILK_LOG_E("Failed to allocate memory for full SQL statement");
        return NULL;
    }

    char* p = full_sql;
    int   param_idx = 0;
    for (const char* c = sql; *c; c++) {
        if (*c == '?' && params[param_idx]) {
            *p++ = '\'';
            for (const char* v = params[param_idx]; *v; v++) {
                if (*v == '\'') {
                    *p++ = '\'';
                    *p++ = '\'';
                } else {
                    *p++ = *v;
                }
            }
            *p++ = '\'';
            param_idx++;
        } else {
            *p++ = *c;
        }
    }
    *p = '\0';

    csilk_mutex_lock(&pool->mutex);
    csilk_json_t* result = csilk_db_query_json_locked(pool, full_sql);
    csilk_mutex_unlock(&pool->mutex);

    uint64_t duration = (csilk_io_hrtime() - start) / 1000;
    atomic_fetch_add(&db_duration_us_total, duration);

    if (!result) {
        atomic_fetch_add(&db_errors_total, 1);
        CSILK_LOG_E("Parameterized database query failed (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    full_sql);
    } else {
        atomic_fetch_add(&db_queries_total, 1);
        CSILK_LOG_T("Parameterized database query succeeded (duration: %.2f ms): %s",
                    (double)duration / 1000.0,
                    full_sql);
    }

    free(full_sql);
    return result;
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
