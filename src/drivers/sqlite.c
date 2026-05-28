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
		fprintf(stderr,
			"csilk_db_sqlite: cannot open '%s': %s\n",
			dsn,
			sqlite3_errmsg(conn->db));
		free(conn);
		return -1;
	}

	pool->connection = conn;
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
	if (!pool || !pool->connection) {
		return -1;
	}

	sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;
	if (conn->db) {
		sqlite3_close(conn->db);
	}
	free(conn);
	pool->connection = NULL;
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
	if (!pool || !pool->connection || !sql || !result) {
		return -1;
	}

	sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;

	sqlite3_stmt* stmt = NULL;
	int rc = sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "csilk_db_sqlite: prepare failed: %s\n", sqlite3_errmsg(conn->db));
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
		result->rows =
		    realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
		if (!result->rows) {
			sqlite3_finalize(stmt);
			sqlite_free_result(result);
			return -1;
		}
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
	if (!pool || !pool->connection || !sql) {
		return -1;
	}

	sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;
	char* err = NULL;
	int rc = sqlite3_exec(conn->db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "csilk_db_sqlite: exec failed: %s\n", err ? err : "unknown");
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
