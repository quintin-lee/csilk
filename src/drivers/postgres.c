#ifdef HAS_POSTGRES
/**
 * @file postgres.c
 * @brief PostgreSQL driver for the csilk pluggable DB interface.
 *
 * Implements the csilk_db_driver_t vtable using libpq.
 * Key design points:
 *   - DSN is passed directly to PQconnectdb in standard PostgreSQL
 *     key=value format (e.g., "host=... port=... dbname=...").
 *   - Queries use PQexec (synchronous, full result buffering).
 *   - nullptr SQL values are detected via PQgetisnull and stored as nullptr
 *     pointers in the result row.
 *   - Transactions use raw SQL sentinel strings (BEGIN/COMMIT/ROLLBACK).
 *
 * @copyright MIT License
 */

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/** @brief Per-connection data for the PostgreSQL driver. */
typedef struct {
	PGconn* db; /**< libpq connection handle. */
} pg_conn_t;

/**
 * @brief Open a PostgreSQL connection using the given DSN.
 *
 * The DSN is passed as-is to PQconnectdb, which accepts the standard
 * libpq key=value format.  Connection status is verified via PQstatus
 * before returning success.
 */
static int
postgres_connect(csilk_db_pool_t* pool, const char* dsn)
{
	if (!pool || !dsn) {
		return -1;
	}

	pg_conn_t* conn = calloc(1, sizeof(pg_conn_t));
	if (!conn) {
		return -1;
	}

	conn->db = PQconnectdb(dsn);
	if (!conn->db) {
		CSILK_LOG_E("csilk_db_postgres: PQconnectdb failed (OOM)");
		free(conn);
		return -1;
	}

	if (PQstatus(conn->db) != CONNECTION_OK) {
		CSILK_LOG_E("csilk_db_postgres: connect failed: %s", PQerrorMessage(conn->db));
		PQfinish(conn->db);
		free(conn);
		return -1;
	}

	csilk_db_pool_set_connection(pool, conn);
	return 0;
}

/**
 * @brief Close the PostgreSQL connection and free the connection struct.
 */
static int
postgres_disconnect(csilk_db_pool_t* pool)
{
	if (!pool || !csilk_db_pool_get_connection(pool)) {
		return -1;
	}

	pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);
	if (conn->db) {
		PQfinish(conn->db);
	}
	free(conn);
	csilk_db_pool_set_connection(pool, nullptr);
	return 0;
}

/**
 * @brief Free all memory associated with a query result set.
 * Iterates rows (freeing each value, the values array, and the row),
 * column names, and top-level arrays.  Resets counts to prevent double-free.
 */
static void
postgres_free_result(csilk_db_result_t* result)
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
	result->rows = nullptr;
	result->column_names = nullptr;
	result->row_count = 0;
	result->column_count = 0;
}

/**
 * @brief Execute a SELECT query and buffer the full result set.
 *
 * Flow:
 *   1. Send the query via PQexec.
 *   2. Check result status: PGRES_TUPLES_OK indicates a query return,
 *      PGRES_COMMAND_OK means no result rows (return empty).
 *   3. Extract column names via PQfname.
 *   4. Iterate rows with PQgetvalue, using PQgetisnull to detect SQL NULLs.
 *   5. On any allocation failure, partial results are freed immediately.
 *
 * @note nullptr SQL values are stored as nullptr pointers in the row's values
 *       array, not as empty strings.
 */
static int
postgres_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
		return -1;
	}

	pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);

	PGresult* pg_res = PQexec(conn->db, sql);
	if (!pg_res) {
		return -1;
	}

	ExecStatusType status = PQresultStatus(pg_res);
	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
		CSILK_LOG_E("csilk_db_postgres: query failed: %s", PQresultErrorMessage(pg_res));
		PQclear(pg_res);
		return -1;
	}

	/* Non-SELECT commands produce no tuples */
	if (status == PGRES_COMMAND_OK) {
		PQclear(pg_res);
		return 0;
	}

	int ntuples = PQntuples(pg_res);
	int nfields = PQnfields(pg_res);

	/* --- Extract column metadata --- */
	result->column_count = nfields;
	result->column_names = calloc(nfields, sizeof(char*));
	if (!result->column_names && nfields > 0) {
		PQclear(pg_res);
		return -1;
	}
	for (int i = 0; i < nfields; i++) {
		result->column_names[i] = strdup(PQfname(pg_res, i));
	}

	/* --- Iterate rows --- */
	for (int i = 0; i < ntuples; i++) {
		csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
		if (!drow) {
			PQclear(pg_res);
			postgres_free_result(result);
			return -1;
		}

		drow->count = nfields;
		drow->values = calloc(nfields, sizeof(char*));
		if (!drow->values) {
			free(drow);
			PQclear(pg_res);
			postgres_free_result(result);
			return -1;
		}

		/* Copy each field; nullptr SQL values remain nullptr pointers */
		for (int j = 0; j < nfields; j++) {
			drow->values[j] =
			    PQgetisnull(pg_res, i, j) ? nullptr : strdup(PQgetvalue(pg_res, i, j));
		}

		/* Append row to result array */
		csilk_db_row_t** new_rows =
		    realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
		if (!new_rows) {
			free(drow->values);
			free(drow);
			PQclear(pg_res);
			postgres_free_result(result);
			return -1;
		}
		result->rows = new_rows;
		result->rows[result->row_count++] = drow;
	}

	PQclear(pg_res);
	return 0;
}

/**
 * @brief Execute a SQL statement that returns no result rows.
 *
 * Sends the statement via PQexec and checks for error status.  Unlike the
 * MySQL driver, no extra result draining is needed since PQexec handles
 * single-statement execution atomically.
 */
static int
postgres_exec(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
		return -1;
	}

	pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);

	PGresult* pg_res = PQexec(conn->db, sql);
	if (!pg_res) {
		return -1;
	}

	ExecStatusType status = PQresultStatus(pg_res);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		CSILK_LOG_E("csilk_db_postgres: exec failed: %s", PQresultErrorMessage(pg_res));
		PQclear(pg_res);
		return -1;
	}

	PQclear(pg_res);
	return 0;
}

/** @brief Begin a transaction (sends "BEGIN"). */
static int
postgres_transaction_begin(csilk_db_pool_t* pool)
{
	return postgres_exec(pool, "BEGIN");
}

/** @brief Commit the current transaction (sends "COMMIT"). */
static int
postgres_transaction_commit(csilk_db_pool_t* pool)
{
	return postgres_exec(pool, "COMMIT");
}

/** @brief Rollback the current transaction (sends "ROLLBACK"). */
static int
postgres_transaction_rollback(csilk_db_pool_t* pool)
{
	return postgres_exec(pool, "ROLLBACK");
}

/** @brief Pre-built driver vtable for PostgreSQL. */
csilk_db_driver_t csilk_db_postgres_driver = {
    .name = "postgres",
    .connect = postgres_connect,
    .disconnect = postgres_disconnect,
    .query = postgres_query,
    .exec = postgres_exec,
    .transaction_begin = postgres_transaction_begin,
    .transaction_commit = postgres_transaction_commit,
    .transaction_rollback = postgres_transaction_rollback,
    .free_result = postgres_free_result,
};

/**
 * @brief Register the PostgreSQL driver with the database subsystem.
 * Only compiled when HAS_POSTGRES is defined.
 */
void
csilk_db_postgres_init(void)
{
	csilk_db_register_driver("postgres", &csilk_db_postgres_driver);
}

#endif /* HAS_POSTGRES */
