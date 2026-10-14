/**
 * @file sqlite.c
 * @brief SQLite3 database driver for csilk.
 *
 * Implements the csilk_db_driver_t vtable using the native sqlite3 C API.
 * Key design points:
 *   - Uses sqlite3_open_v2 with SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
 *     (the database file is created if it does not exist).
 *   - SELECT queries use sqlite3_prepare_v2 + sqlite3_step for row-by-row
 *     iteration; the full result set is buffered before returning.
 *   - Non-query statements (INSERT, UPDATE, DELETE, DDL) use the simpler
 *     sqlite3_exec callback-free interface.
 *   - The DSN is simply a filesystem path to the .db file.
 *
 * @copyright MIT License
 */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "db_internal.h"

/** @brief Per-connection data for the SQLite driver. */
typedef struct {
    sqlite3* db;
} sqlite_conn_t;

/** @brief Open a connection to a SQLite database file.
 *
 * Uses sqlite3_open_v2 with read/write+create flags. Stores the connection
 * handle in the pool. On failure, frees the allocated connection and returns
 * an error.
 *
 * @param pool The database pool to initialize.
 * @param dsn  Filesystem path to the SQLite database file.
 * @return 0 on success, -1 if parameters are invalid or open fails. */
static int
sqlite_connect(csilk_db_pool_t* pool, const char* dsn)
{
    if (!pool || !dsn) {
        return -1;
    }

    sqlite_conn_t* conn = calloc(1, sizeof(sqlite_conn_t));
    if (!conn) {
        return -1;
    }

    int rc = sqlite3_open_v2(dsn, &conn->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        CSILK_LOG_E("csilk_db_sqlite: cannot open '%s': %s", dsn, sqlite3_errmsg(conn->db));
        free(conn);
        return -1;
    }

    csilk_db_pool_set_connection(pool, conn);
    return 0;
}

/** @brief Close a SQLite connection and free the pool's connection data.
 *
 * Calls sqlite3_close() on the underlying handle, frees the connection struct,
 * and sets pool->connection to NULL.
 *
 * @param pool The database pool to shut down.
 * @return 0 on success, -1 if pool or its connection is NULL. */
static int
sqlite_disconnect(csilk_db_pool_t* pool)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return -1;
    }

    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(pool);
    if (conn->db) {
        sqlite3_close(conn->db);
    }
    free(conn);
    csilk_db_pool_set_connection(pool, NULL);
    return 0;
}

/** @brief Free all memory associated with a query result set.
 *
 * Iterates rows, column names, and the top-level arrays, freeing each
 * allocation.  Resets row_count and column_count to 0 after cleanup.
 *
 * @note Currently frees row structs directly without freeing the
 *       row->values[] array or its individual string elements.
 *
 * @param result The result set to free (may be NULL). */
static void
sqlite_free_result(csilk_db_result_t* result)
{
    if (!result) {
        return;
    }
    if (result->rows) {
        for (int i = 0; i < result->row_count; i++) {
            csilk_db_row_t* row = result->rows[i];
            if (row) {
                for (int j = 0; j < row->count; j++) {
                    free(row->values[j]);
                }
                free(row->values);
                free(row);
            }
        }
        free(result->rows);
    }
    for (int i = 0; i < result->column_count; i++) {
        free(result->column_names[i]);
    }
    free(result->column_names);
    result->rows = NULL;
    result->column_names = NULL;
    result->row_count = 0;
    result->column_count = 0;
}

/** @brief Execute a SQL query and return the full result set.
 *
 * Prepares the statement, retrieves column names, then steps through all
 * rows, allocating each row's values array. On any allocation failure,
 * partial results are freed and -1 is returned.
 *
 * @param pool   The database pool (must be connected).
 * @param sql    SQL query string.
 * @param result [out] Populated result set (must be freed with
 *               sqlite_free_result).
 * @return 0 on success, -1 on error. */
/**
 * @brief Execute a SELECT query and buffer the full result set.
 *
 * Flow:
 *   1. Prepare the statement via sqlite3_prepare_v2.
 *   2. Extract column metadata from the prepared statement.
 *   3. Step through rows with sqlite3_step, allocating a csilk_db_row_t
 *      per row.  Each cell is extracted via sqlite3_column_text and
 *      duplicated via strdup (NULL cells become empty strings).
 *   4. Finalize the statement.  On any allocation failure, partial
 *      results are freed and -1 is returned.
 */
static int
sqlite_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
        return -1;
    }

    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(pool);

    sqlite3_stmt* stmt = NULL;
    int           rc = sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        CSILK_LOG_E("csilk_db_sqlite: prepare failed: %s", sqlite3_errmsg(conn->db));
        return -1;
    }

    /* --- Extract column metadata before stepping --- */
    result->row_count = 0;
    result->column_count = sqlite3_column_count(stmt);
    result->column_names = calloc(result->column_count, sizeof(char*));
    if (!result->column_names && result->column_count > 0) {
        sqlite3_finalize(stmt);
        return -1;
    }
    for (int i = 0; i < result->column_count; i++) {
        result->column_names[i] = strdup(sqlite3_column_name(stmt, i));
    }

    /* --- Step through all result rows --- */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
        if (!row) {
            sqlite3_finalize(stmt);
            sqlite_free_result(result);
            return -1;
        }

        row->count = result->column_count;
        row->values = calloc(result->column_count, sizeof(char*));
        if (!row->values) {
            free(row);
            sqlite3_finalize(stmt);
            sqlite_free_result(result);
            return -1;
        }

        /* Copy each column as text; NULL values become "" for consistency */
        for (int i = 0; i < result->column_count; i++) {
            const char* val = (const char*)sqlite3_column_text(stmt, i);
            row->values[i] = val ? strdup(val) : strdup("");
        }

        /* Append row to the dynamic result array */
        csilk_db_row_t** new_rows =
            realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
        if (!new_rows) {
            /* realloc failed: free the not-yet-stored row to avoid a leak,
             * then release the partial result set. */
            for (int i = 0; i < row->count; i++) {
                free(row->values[i]);
            }
            free(row->values);
            free(row);
            sqlite3_finalize(stmt);
            sqlite_free_result(result);
            return -1;
        }
        result->rows = new_rows;
        result->rows[result->row_count++] = row;
    }

    sqlite3_finalize(stmt);
    return 0;
}

/** @brief Execute a SQL statement that returns no result rows.
 *
 * Uses sqlite3_exec() for DDL/INSERT/UPDATE/DELETE statements.
 *
 * @param pool The database pool (must be connected).
 * @param sql  SQL statement string.
 * @return 0 on success, -1 on error. */
/**
 * @brief Execute a SQL statement that returns no result rows.
 *
 * Uses sqlite3_exec with no callback for DDL/INSERT/UPDATE/DELETE.
 * The callback-free invocation discards any output rows silently.
 * On error, the error message from sqlite3_errmsg is freed after logging.
 */
static int
sqlite_exec(csilk_db_pool_t* pool, const char* sql)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
        return -1;
    }

    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(pool);
    char*          err = NULL;
    int            rc = sqlite3_exec(conn->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        CSILK_LOG_E("csilk_db_sqlite: exec failed: %s", err ? err : "unknown");
        if (err) {
            sqlite3_free(err);
        }
        return -1;
    }
    return 0;
}

/** @brief Begin a SQLite transaction (delegates to sqlite_exec).
 *
 * @param pool The database pool.
 * @return 0 on success, -1 on error. */
static int
sqlite_transaction_begin(csilk_db_pool_t* pool)
{
    return sqlite_exec(pool, "BEGIN TRANSACTION");
}

/** @brief Commit the current SQLite transaction.
 *
 * @param pool The database pool.
 * @return 0 on success, -1 on error. */
static int
sqlite_transaction_commit(csilk_db_pool_t* pool)
{
    return sqlite_exec(pool, "COMMIT");
}

/** @brief Rollback the current SQLite transaction.
 *
 * @param pool The database pool.
 * @return 0 on success, -1 on error. */
static int
sqlite_transaction_rollback(csilk_db_pool_t* pool)
{
    return sqlite_exec(pool, "ROLLBACK");
}

static int
sqlite_in_transaction(csilk_db_pool_t* pool)
{
    void* raw_conn = csilk_db_pool_get_connection(pool);
    if (!raw_conn) {
        return 0;
    }
    sqlite_conn_t* conn = (sqlite_conn_t*)raw_conn;
    if (!conn->db) {
        return 0;
    }
    return (sqlite3_get_autocommit(conn->db) == 0) ? 1 : 0;
}

static int
sqlite_stmt_prepare(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt)
{
    if (!pool || !sql || !out_stmt) {
        return -1;
    }
    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn || !conn->db) {
        return -1;
    }

    sqlite3_stmt* s = NULL;
    int           rc = sqlite3_prepare_v2(conn->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) {
        CSILK_LOG_E("sqlite_stmt_prepare failed: %s (sql: %s)", sqlite3_errmsg(conn->db), sql);
        return -1;
    }

    csilk_stmt_t* stmt = calloc(1, sizeof(csilk_stmt_t));
    if (!stmt) {
        sqlite3_finalize(s);
        return -1;
    }
    stmt->pool = pool;
    stmt->handle = s;
    *out_stmt = stmt;
    return 0;
}

static int
sqlite_stmt_bind_int64(csilk_stmt_t* stmt, int index, int64_t val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    int rc = sqlite3_bind_int64((sqlite3_stmt*)stmt->handle, index, val);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static int
sqlite_stmt_bind_double(csilk_stmt_t* stmt, int index, double val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    int rc = sqlite3_bind_double((sqlite3_stmt*)stmt->handle, index, val);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static int
sqlite_stmt_bind_text(csilk_stmt_t* stmt, int index, const char* val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    int rc =
        sqlite3_bind_text((sqlite3_stmt*)stmt->handle, index, val ? val : "", -1, SQLITE_TRANSIENT);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static int
sqlite_stmt_bind_null(csilk_stmt_t* stmt, int index)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    int rc = sqlite3_bind_null((sqlite3_stmt*)stmt->handle, index);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static int
sqlite_stmt_execute(csilk_stmt_t* stmt, int64_t* out_affected_rows)
{
    if (!stmt || !stmt->handle || !stmt->pool) {
        return -1;
    }
    sqlite3_stmt*  s = (sqlite3_stmt*)stmt->handle;
    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(stmt->pool);

    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        CSILK_LOG_E("sqlite_stmt_execute failed: %s", conn ? sqlite3_errmsg(conn->db) : "unknown");
        sqlite3_reset(s);
        return -1;
    }

    if (out_affected_rows && conn && conn->db) {
        *out_affected_rows = sqlite3_changes(conn->db);
    }
    sqlite3_reset(s);
    return 0;
}

static csilk_json_t*
sqlite_stmt_query_json(csilk_stmt_t* stmt)
{
    if (!stmt || !stmt->handle) {
        return NULL;
    }
    sqlite3_stmt* s = (sqlite3_stmt*)stmt->handle;
    csilk_json_t* arr = csilk_json_array();
    if (!arr) {
        return NULL;
    }

    int rc = 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        csilk_json_t* row = csilk_json_object();
        int           col_count = sqlite3_column_count(s);
        for (int i = 0; i < col_count; i++) {
            const char* col_name = sqlite3_column_name(s, i);
            int         col_type = sqlite3_column_type(s, i);
            switch (col_type) {
            case SQLITE_INTEGER:
                csilk_json_add_int(row, col_name, sqlite3_column_int64(s, i));
                break;
            case SQLITE_FLOAT:
                csilk_json_add_number(row, col_name, sqlite3_column_double(s, i));
                break;
            case SQLITE_NULL:
                csilk_json_add_null(row, col_name);
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB:
            default: {
                const unsigned char* txt = sqlite3_column_text(s, i);
                csilk_json_add_string(row, col_name, txt ? (const char*)txt : "");
                break;
            }
            }
        }
        csilk_json_array_append(arr, row);
    }

    sqlite3_reset(s);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        csilk_json_free(arr);
        return NULL;
    }
    return arr;
}

static void
sqlite_stmt_close(csilk_stmt_t* stmt)
{
    if (!stmt) {
        return;
    }
    if (stmt->handle) {
        sqlite3_finalize((sqlite3_stmt*)stmt->handle);
        stmt->handle = NULL;
    }
    free(stmt);
}

/** @brief Pre-built driver vtable for the SQLite3 database backend.
 *
 * Registered automatically when csilk_db_sqlite_init() is called. Users
 * can also reference this struct directly to register manually. */
csilk_db_driver_t csilk_db_sqlite_driver = {
    .name = "sqlite",
    .connect = sqlite_connect,
    .disconnect = sqlite_disconnect,
    .query = sqlite_query,
    .exec = sqlite_exec,
    .transaction_begin = sqlite_transaction_begin,
    .transaction_commit = sqlite_transaction_commit,
    .transaction_rollback = sqlite_transaction_rollback,
    .in_transaction = sqlite_in_transaction,
    .stmt_prepare = sqlite_stmt_prepare,
    .stmt_bind_int64 = sqlite_stmt_bind_int64,
    .stmt_bind_double = sqlite_stmt_bind_double,
    .stmt_bind_text = sqlite_stmt_bind_text,
    .stmt_bind_null = sqlite_stmt_bind_null,
    .stmt_execute = sqlite_stmt_execute,
    .stmt_query_json = sqlite_stmt_query_json,
    .stmt_close = sqlite_stmt_close,
    .free_result = sqlite_free_result,
};

/** @brief Initialize and register the SQLite database driver with csilk.
 *
 * Call this at startup (e.g., in the app init callback) to make the "sqlite"
 * driver available for csilk_db_create() calls.
 *
 * @note This function is idempotent-safe but typically called once. */
void
csilk_db_sqlite_init(void)
{
    csilk_db_register_driver("sqlite", &csilk_db_sqlite_driver);
}
