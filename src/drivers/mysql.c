#ifdef HAS_MYSQL
/**
 * @file mysql.c
 * @brief MySQL database driver for the csilk pluggable DB interface.
 *
 * Implements the csilk_db_driver_t vtable using libmysqlclient.
 * Key design points:
 *   - DSN format: "host=...;port=...;user=...;password=...;dbname=..."
 *   - Uses mysql_real_connect for TCP connections (no Unix socket support yet).
 *   - Query results are fully buffered via mysql_store_result.
 *   - Transactions use raw SQL sentinel strings (BEGIN/COMMIT/ROLLBACK).
 *
 * @copyright MIT License
 */

#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/* strndup is POSIX.1-2008 but may not be visible on musl-based systems
 * (Alpine Linux) without the correct feature test macros. We always
 * use this local wrapper instead of calling strndup directly. */
static inline char*
csilk_strndup(const char* s, size_t n)
{
	size_t len = strnlen(s, n);
	char* copy = (char*)malloc(len + 1);
	if (copy) {
		memcpy(copy, s, len);
		copy[len] = '\0';
	}
	return copy;
}

/** @brief Per-connection data for the MySQL driver. */
typedef struct {
	MYSQL* db; /**< libmysqlclient connection handle. */
} mysql_conn_t;

/**
 * @brief Parse a semicolon-delimited DSN string into connection parameters.
 *
 * Expected format (all fields optional except as noted):
 *   "host=...;port=...;user=...;password=...;dbname=..."
 * Port defaults to 3306 if omitted.  Unknown keys are silently ignored.
 * The input string is duplicated internally for tokenisation.
 *
 * @param dsn      Raw DSN string (may be nullptr).
 * @param host     [out] Heap-allocated host string (or nullptr).
 * @param port     [out] Port number (defaults to 3306).
 * @param user     [out] Heap-allocated username (or nullptr).
 * @param password [out] Heap-allocated password (or nullptr).
 * @param dbname   [out] Heap-allocated database name (or nullptr).
 */
static void
mysql_parse_dsn(
    const char* dsn, char** host, int* port, char** user, char** password, char** dbname)
{
	*host = nullptr;
	*port = 3306;
	*user = nullptr;
	*password = nullptr;
	*dbname = nullptr;
	if (!dsn) {
		return;
	}

	char* buf = strdup(dsn);
	if (!buf) {
		return;
	}

	char* token = strtok(buf, ";");
	while (token) {
		while (*token == ' ') {
			token++;
		}
		char* eq = strchr(token, '=');
		if (!eq) {
			token = strtok(nullptr, ";");
			continue;
		}
		*eq = '\0';
		const char* key = token;
		const char* val = eq + 1;

		if (strcmp(key, "host") == 0) {
			*host = strdup(val);
		} else if (strcmp(key, "port") == 0) {
			*port = atoi(val);
		} else if (strcmp(key, "user") == 0) {
			*user = strdup(val);
		} else if (strcmp(key, "password") == 0) {
			*password = strdup(val);
		} else if (strcmp(key, "dbname") == 0) {
			*dbname = strdup(val);
		}

		token = strtok(nullptr, ";");
	}
	free(buf);
}

/**
 * @brief Open a MySQL connection using the given DSN.
 *
 * Allocates a connection struct, initialises the MYSQL handle, parses the DSN,
 * and calls mysql_real_connect.  On failure the connection and struct are
 * cleaned up before returning -1.
 */
static int
mysql_drv_connect(csilk_db_pool_t* pool, const char* dsn)
{
	if (!pool || !dsn) {
		return -1;
	}

	mysql_conn_t* conn = calloc(1, sizeof(mysql_conn_t));
	if (!conn) {
		return -1;
	}

	conn->db = mysql_init(nullptr);
	if (!conn->db) {
		CSILK_LOG_E("csilk_db_mysql: mysql_init failed");
		free(conn);
		return -1;
	}

	char *host = nullptr, *user = nullptr, *password = nullptr, *dbname = nullptr;
	int port = 3306;
	mysql_parse_dsn(dsn, &host, &port, &user, &password, &dbname);

	/* TCP connection; no client flags set */
	MYSQL* ret = mysql_real_connect(conn->db, host, user, password, dbname, port, nullptr, 0);
	free(host);
	free(user);
	free(password);
	free(dbname);

	if (!ret) {
		CSILK_LOG_E("csilk_db_mysql: connect failed: %s", mysql_error(conn->db));
		mysql_close(conn->db);
		free(conn);
		return -1;
	}

	csilk_db_pool_set_connection(pool, conn);
	return 0;
}

/**
 * @brief Close the MySQL connection and free the connection struct.
 */
static int
mysql_drv_disconnect(csilk_db_pool_t* pool)
{
	if (!pool || !csilk_db_pool_get_connection(pool)) {
		return -1;
	}

	mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);
	if (conn->db) {
		mysql_close(conn->db);
	}
	free(conn);
	csilk_db_pool_set_connection(pool, nullptr);
	return 0;
}

/**
 * @brief Free all memory associated with a query result set.
 *
 * Iterates over rows (freeing each value string, the values array, and the
 * row struct), then frees column names and the top-level arrays.  Counts
 * are reset to zero to prevent double-free.
 */
static void
mysql_free_csilk_result(csilk_db_result_t* result)
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
 *   1. Execute via mysql_real_query.
 *   2. Buffer the entire result with mysql_store_result (all rows in memory).
 *      If the query produces no result set (e.g. a non-SELECT), return 0
 *      with an empty result.
 *   3. Extract column names from the MYSQL_FIELD metadata.
 *   4. Iterate mysql_fetch_row, allocating a csilk_db_row_t per row.
 *      Each cell is duplicated via strndup (respecting binary-safe lengths).
 *   5. On any allocation failure, partial results are freed immediately.
 *
 * @note Uses strndup with mysql_fetch_lengths for binary-safe field copies.
 *       nullptr SQL values remain nullptr in the result row.
 */
static int
mysql_drv_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
		return -1;
	}

	mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);

	if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
		CSILK_LOG_E("csilk_db_mysql: query failed: %s", mysql_error(conn->db));
		return -1;
	}

	/* Buffer the entire result set client-side */
	MYSQL_RES* mysql_res = mysql_store_result(conn->db);
	if (!mysql_res) {
		return 0;
	}

	/* --- Extract column metadata --- */
	MYSQL_FIELD* fields = mysql_fetch_fields(mysql_res);
	unsigned int num_fields = mysql_num_fields(mysql_res);

	result->column_count = (int)num_fields;
	result->column_names = calloc(num_fields, sizeof(char*));
	if (!result->column_names) {
		mysql_free_result(mysql_res);
		return -1;
	}
	for (unsigned int i = 0; i < num_fields; i++) {
		result->column_names[i] = strdup(fields[i].name);
	}

	/* --- Iterate rows --- */
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(mysql_res))) {
		unsigned long* lengths = mysql_fetch_lengths(mysql_res);

		csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
		if (!drow) {
			mysql_free_result(mysql_res);
			mysql_free_csilk_result(result);
			return -1;
		}

		drow->count = (int)num_fields;
		drow->values = calloc(num_fields, sizeof(char*));
		if (!drow->values) {
			free(drow);
			mysql_free_result(mysql_res);
			mysql_free_csilk_result(result);
			return -1;
		}

		/* Copy each field using the binary-safe length from mysql_fetch_lengths */
		for (unsigned int i = 0; i < num_fields; i++) {
			drow->values[i] = row[i] ? csilk_strndup(row[i], lengths[i]) : nullptr;
		}

		/* Append row to the result array */
		csilk_db_row_t** new_rows =
		    realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
		if (!new_rows) {
			free(drow->values);
			free(drow);
			mysql_free_result(mysql_res);
			mysql_free_csilk_result(result);
			return -1;
		}
		result->rows = new_rows;
		result->rows[result->row_count++] = drow;
	}

	mysql_free_result(mysql_res);
	return 0;
}

/**
 * @brief Execute a SQL statement that returns no result rows.
 *
 * After execution, any result set is consumed and discarded via
 * mysql_store_result + mysql_free_result.  This handles cases where
 * the statement produces rows even though the caller expects none.
 */
static int
mysql_drv_exec(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
		return -1;
	}

	mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);

	if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
		CSILK_LOG_E("csilk_db_mysql: exec failed: %s", mysql_error(conn->db));
		return -1;
	}

	/* Drain any remaining result (important for multi-statement or
   * statements that unexpectedly produce rows) */
	MYSQL_RES* res = mysql_store_result(conn->db);
	if (res) {
		mysql_free_result(res);
	}

	return 0;
}

/** @brief Begin a transaction (sends "BEGIN"). */
static int
mysql_drv_transaction_begin(csilk_db_pool_t* pool)
{
	return mysql_drv_exec(pool, "BEGIN");
}

/** @brief Commit the current transaction (sends "COMMIT"). */
static int
mysql_drv_transaction_commit(csilk_db_pool_t* pool)
{
	return mysql_drv_exec(pool, "COMMIT");
}

/** @brief Rollback the current transaction (sends "ROLLBACK"). */
static int
mysql_drv_transaction_rollback(csilk_db_pool_t* pool)
{
	return mysql_drv_exec(pool, "ROLLBACK");
}

/** @brief Pre-built driver vtable for MySQL. */
csilk_db_driver_t csilk_db_mysql_driver = {
    .name = "mysql",
    .connect = mysql_drv_connect,
    .disconnect = mysql_drv_disconnect,
    .query = mysql_drv_query,
    .exec = mysql_drv_exec,
    .transaction_begin = mysql_drv_transaction_begin,
    .transaction_commit = mysql_drv_transaction_commit,
    .transaction_rollback = mysql_drv_transaction_rollback,
    .free_result = mysql_free_csilk_result,
};

/**
 * @brief Register the MySQL driver with the database subsystem.
 * Only compiled when HAS_MYSQL is defined.
 */
void
csilk_db_mysql_init(void)
{
	csilk_db_register_driver("mysql", &csilk_db_mysql_driver);
}

#endif /* HAS_MYSQL */
