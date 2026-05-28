#ifdef HAS_REDIS
/**
 * @file redis.c
 * @brief Redis database driver for the csilk pluggable DB interface.
 *
 * Implements the csilk_db_driver_t vtable using hiredis (hiredis.org).
 * Key design points:
 *   - DSN format: "host=...;port=...;password=...;db=..."
 *   - Uses redisConnectWithTimeout for TCP connections (default
 * 127.0.0.1:6379).
 *   - The `query` function accepts arbitrary Redis commands (GET, HGETALL,
 * KEYS, SMEMBERS, LRANGE, etc.) and maps reply types to tabular result sets:
 *     - REDIS_REPLY_STRING  → single row, one column "value"
 *     - REDIS_REPLY_ARRAY   → one row per element; hash entries produce
 *       ("field", "value") columns, flat arrays produce one "value" column
 *     - REDIS_REPLY_STATUS  → converted to a single "OK" row
 *   - The `exec` function runs non-query commands (SET, DEL, EXPIRE, etc.)
 *     and returns 0 on success (-1 on error).
 *   - Transactions use the native MULTI/EXEC/DISCARD commands.
 *
 * @note Redis is a key-value store, not a relational database.  The tabular
 *       result mapping is a best-effort approximation — complex nested
 *       structures (e.g. RESP3 maps) are not supported.
 *
 * @copyright MIT License
 */

#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/** @brief Per-connection data for the Redis driver. */
typedef struct {
	redisContext* c; /**< hiredis connection context. */
} redis_conn_t;

/**
 * @brief Parse a semicolon-delimited DSN string into connection parameters.
 *
 * Expected format (all fields optional except as noted):
 *   "host=...;port=...;password=...;db=..."
 * Host defaults to "127.0.0.1", port defaults to 6379, db defaults to 0.
 * Unknown keys are silently ignored.  The input string is duplicated
 * internally for tokenisation.
 *
 * @param dsn      Raw DSN string (may be NULL).
 * @param host     [out] Heap-allocated host string (or NULL = default).
 * @param port     [out] Port number (defaults to 6379).
 * @param password [out] Heap-allocated password (or NULL = no auth).
 * @param db       [out] Database index (defaults to 0).
 */
static void
redis_parse_dsn(const char* dsn, char** host, int* port, char** password, int* db)
{
	*host = NULL;
	*port = 6379;
	*password = NULL;
	*db = 0;
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
			token = strtok(NULL, ";");
			continue;
		}
		*eq = '\0';
		const char* key = token;
		const char* val = eq + 1;

		if (strcmp(key, "host") == 0) {
			*host = strdup(val);
		} else if (strcmp(key, "port") == 0) {
			*port = atoi(val);
		} else if (strcmp(key, "password") == 0) {
			*password = strdup(val);
		} else if (strcmp(key, "db") == 0) {
			*db = atoi(val);
		}

		token = strtok(NULL, ";");
	}
	free(buf);
}

/**
 * @brief Open a Redis connection using the given DSN.
 *
 * Allocates a connection struct, parses the DSN, calls redisConnectWithTimeout
 * with a 5-second timeout, authenticates if a password is provided, and
 * selects the database index.  On failure, the connection and struct are
 * cleaned up before returning -1.
 *
 * @param pool The database pool to initialize.
 * @param dsn  DSN string in "host=...;port=...;password=...;db=..." format.
 * @return 0 on success, -1 if parameters are invalid or connect fails. */
static int
redis_drv_connect(csilk_db_pool_t* pool, const char* dsn)
{
	if (!pool || !dsn) {
		return -1;
	}

	char *host = NULL, *password = NULL;
	int port = 6379, db = 0;
	redis_parse_dsn(dsn, &host, &port, &password, &db);

	redis_conn_t* conn = calloc(1, sizeof(redis_conn_t));
	if (!conn) {
		free(host);
		free(password);
		return -1;
	}

	/* Connect with 5-second timeout */
	struct timeval tv = {5, 0};
	conn->c = redisConnectWithTimeout(host ? host : "127.0.0.1", port, tv);
	free(host);

	if (!conn->c || conn->c->err) {
		fprintf(stderr,
			"csilk_db_redis: connect failed: %s\n",
			conn->c ? conn->c->errstr : "allocation failed");
		if (conn->c) {
			redisFree(conn->c);
		}
		free(conn);
		free(password);
		return -1;
	}

	/* Authenticate if password provided */
	if (password) {
		redisReply* reply = redisCommand(conn->c, "AUTH %s", password);
		free(password);
		if (!reply || reply->type == REDIS_REPLY_ERROR) {
			fprintf(stderr,
				"csilk_db_redis: AUTH failed: %s\n",
				reply ? reply->str : "no reply");
			freeReplyObject(reply);
			redisFree(conn->c);
			free(conn);
			return -1;
		}
		freeReplyObject(reply);
	}

	/* Select database index */
	if (db > 0) {
		redisReply* reply = redisCommand(conn->c, "SELECT %d", db);
		if (!reply || reply->type == REDIS_REPLY_ERROR) {
			fprintf(stderr,
				"csilk_db_redis: SELECT %d failed: %s\n",
				db,
				reply ? reply->str : "no reply");
			freeReplyObject(reply);
			redisFree(conn->c);
			free(conn);
			return -1;
		}
		freeReplyObject(reply);
	}

	pool->connection = conn;
	return 0;
}

/**
 * @brief Close the Redis connection and free the connection struct.
 *
 * Calls redisFree on the underlying handle, frees the connection struct,
 * and sets pool->connection to NULL.
 *
 * @param pool The database pool to shut down.
 * @return 0 on success, -1 if pool or its connection is NULL. */
static int
redis_drv_disconnect(csilk_db_pool_t* pool)
{
	if (!pool || !pool->connection) {
		return -1;
	}

	redis_conn_t* conn = (redis_conn_t*)pool->connection;
	if (conn->c) {
		redisFree(conn->c);
	}
	free(conn);
	pool->connection = NULL;
	return 0;
}

/**
 * @brief Free all memory associated with a query result set.
 *
 * Iterates over rows (freeing each value string, the values array, and the
 * row struct), then frees column names and the top-level arrays.  Counts
 * are reset to zero to prevent double-free.
 *
 * @param result The result set to free (may be NULL). */
static void
redis_free_csilk_result(csilk_db_result_t* result)
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

/**
 * @brief Convert a single redisReply into one row with the given column name.
 *
 * Reads the reply's string value (str field) and stores it as the first
 * value of a newly allocated csilk_db_row_t.
 *
 * @param reply       The redisReply to extract from (must be REDIS_REPLY_STRING
 *                    or REDIS_REPLY_STATUS).
 * @param col_name    Column name for the single column.
 * @param col_count   Total number of columns (typically 1).
 * @param row_idx     Row index for the value array (typically 0).
 * @return A populated csilk_db_row_t, or NULL on allocation failure. */
static csilk_db_row_t*
redis_reply_to_row(const redisReply* reply, const char* col_name, int col_count, int row_idx)
{
	(void)col_name;
	(void)row_idx;
	csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
	if (!row) {
		return NULL;
	}

	row->count = col_count;
	row->values = calloc(col_count, sizeof(char*));
	if (!row->values) {
		free(row);
		return NULL;
	}

	if (reply->str) {
		row->values[0] = strdup(reply->str);
	} else {
		row->values[0] = strdup(""); /* NULL string → empty string */
	}
	return row;
}

/**
 * @brief Execute a Redis command and return the result as a tabular result set.
 *
 * This function accepts arbitrary Redis commands (GET, HGETALL, KEYS,
 * SMEMBERS, LRANGE, etc.) via the @p sql parameter and maps the reply to
 * csilk_db_result_t:
 *
 * | Reply Type    | Row Layout | Columns | Example Command  |
 * |---------------|-----------|---------|------------------|
 * | STRING/STATUS | 1 row     | value   | GET mykey        |
 * | ARRAY (flat)  | N rows    | value   | KEYS *, LRANGE   |
 * | ARRAY (hash)  | N/2 rows  | field, value | HGETALL myhash |
 * | INTEGER       | 1 row     | value   | EXISTS mykey     |
 * | NIL           | 0 rows    | (empty) | GET missing      |
 *
 * On any allocation failure, partial results are freed and -1 is returned.
 *
 * @param pool   The database pool (must be connected).
 * @param sql    Redis command string (e.g., "GET mykey").
 * @param result [out] Populated result set (must be freed with
 *               redis_free_csilk_result).
 * @return 0 on success, -1 on error. */
static int
redis_drv_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
	if (!pool || !pool->connection || !sql || !result) {
		return -1;
	}

	redis_conn_t* conn = (redis_conn_t*)pool->connection;
	redisReply* reply = redisCommand(conn->c, sql);
	if (!reply) {
		fprintf(stderr,
			"csilk_db_redis: connection error: %s\n",
			conn->c ? conn->c->errstr : "unknown");
		return -1;
	}

	if (reply->type == REDIS_REPLY_ERROR) {
		fprintf(stderr, "csilk_db_redis: command error: %s\n", reply->str);
		freeReplyObject(reply);
		return -1;
	}

	/* Determine column layout based on reply type */
	int is_hash = 0;
	if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0) {
		/* HGETALL returns alternating field/value pairs → even count */
		is_hash = (reply->elements % 2 == 0) &&
			  reply->element[0]->type == REDIS_REPLY_STRING &&
			  reply->element[1]->type == REDIS_REPLY_STRING;
	}

	switch (reply->type) {
	case REDIS_REPLY_STRING:
	case REDIS_REPLY_STATUS: {
		/* Single string/status reply → one row, one column */
		result->column_count = 1;
		result->column_names = calloc(1, sizeof(char*));
		if (!result->column_names) {
			freeReplyObject(reply);
			return -1;
		}
		result->column_names[0] = strdup("value");

		csilk_db_row_t* row = redis_reply_to_row(reply, "value", 1, 0);
		if (!row) {
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}
		result->rows = malloc(sizeof(csilk_db_row_t*));
		if (!result->rows) {
			free(row->values);
			free(row);
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}
		result->rows[0] = row;
		result->row_count = 1;
		break;
	}

	case REDIS_REPLY_INTEGER: {
		/* Integer reply → one row, "value" column holds the stringified number */
		char buf[32];
		snprintf(buf, sizeof(buf), "%lld", reply->integer);

		result->column_count = 1;
		result->column_names = calloc(1, sizeof(char*));
		if (!result->column_names) {
			freeReplyObject(reply);
			return -1;
		}
		result->column_names[0] = strdup("value");

		csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
		if (!row) {
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}
		row->count = 1;
		row->values = calloc(1, sizeof(char*));
		if (!row->values) {
			free(row);
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}
		row->values[0] = strdup(buf);

		result->rows = malloc(sizeof(csilk_db_row_t*));
		if (!result->rows) {
			free(row->values);
			free(row);
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}
		result->rows[0] = row;
		result->row_count = 1;
		break;
	}

	case REDIS_REPLY_ARRAY: {
		/* Array reply: determine column layout from data pattern */
		int col_count = is_hash ? 2 : 1;
		result->column_count = col_count;
		result->column_names = calloc(col_count, sizeof(char*));
		if (!result->column_names) {
			freeReplyObject(reply);
			return -1;
		}
		if (is_hash) {
			result->column_names[0] = strdup("field");
			result->column_names[1] = strdup("value");
		} else {
			result->column_names[0] = strdup("value");
		}

		result->row_count = is_hash ? (int)(reply->elements / 2) : (int)reply->elements;
		result->rows = calloc(result->row_count, sizeof(csilk_db_row_t*));
		if (!result->rows && result->row_count > 0) {
			free(result->column_names);
			freeReplyObject(reply);
			return -1;
		}

		if (is_hash) {
			/* HGETALL: alternating field/value pairs */
			int ri = 0;
			for (size_t i = 0; i + 1 < reply->elements; i += 2) {
				csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
				if (!row) {
					redis_free_csilk_result(result);
					freeReplyObject(reply);
					return -1;
				}
				row->count = 2;
				row->values = calloc(2, sizeof(char*));
				if (!row->values) {
					free(row);
					redis_free_csilk_result(result);
					freeReplyObject(reply);
					return -1;
				}
				row->values[0] = reply->element[i]->str
						     ? strdup(reply->element[i]->str)
						     : strdup("");
				row->values[1] = reply->element[i + 1]->str
						     ? strdup(reply->element[i + 1]->str)
						     : strdup("");
				result->rows[ri++] = row;
			}
		} else {
			/* Flat array: one column per element */
			for (size_t i = 0; i < reply->elements; i++) {
				csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
				if (!row) {
					redis_free_csilk_result(result);
					freeReplyObject(reply);
					return -1;
				}
				row->count = 1;
				row->values = calloc(1, sizeof(char*));
				if (!row->values) {
					free(row);
					redis_free_csilk_result(result);
					freeReplyObject(reply);
					return -1;
				}

				const redisReply* elem = reply->element[i];
				if (elem->type == REDIS_REPLY_STRING && elem->str) {
					row->values[0] = strdup(elem->str);
				} else if (elem->type == REDIS_REPLY_INTEGER) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%lld", elem->integer);
					row->values[0] = strdup(buf);
				} else {
					row->values[0] = strdup("");
				}
				result->rows[i] = row;
			}
		}
		break;
	}

	case REDIS_REPLY_NIL: {
		/* Nil reply → empty result set (0 rows) */
		result->column_count = 1;
		result->column_names = calloc(1, sizeof(char*));
		if (!result->column_names) {
			freeReplyObject(reply);
			return -1;
		}
		result->column_names[0] = strdup("value");
		result->row_count = 0;
		result->rows = NULL;
		break;
	}

	default: {
		/* Unsupported reply type → treat as empty */
		result->column_count = 0;
		result->row_count = 0;
		result->rows = NULL;
		result->column_names = NULL;
		break;
	}
	}

	freeReplyObject(reply);
	return 0;
}

/**
 * @brief Execute a Redis command that returns no result rows.
 *
 * Runs the given command via redisCommand and discards the reply.
 * Returns 0 if the reply type is not REDIS_REPLY_ERROR.
 *
 * @param pool The database pool (must be connected).
 * @param sql  Redis command string (e.g., "SET mykey myvalue").
 * @return 0 on success, -1 on error. */
static int
redis_drv_exec(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !pool->connection || !sql) {
		return -1;
	}

	redis_conn_t* conn = (redis_conn_t*)pool->connection;
	redisReply* reply = redisCommand(conn->c, sql);
	if (!reply) {
		fprintf(stderr,
			"csilk_db_redis: exec connection error: %s\n",
			conn->c ? conn->c->errstr : "unknown");
		return -1;
	}

	int rc = (reply->type == REDIS_REPLY_ERROR) ? -1 : 0;
	if (rc != 0) {
		fprintf(stderr, "csilk_db_redis: exec error: %s\n", reply->str);
	}
	freeReplyObject(reply);
	return rc;
}

/** @brief Begin a Redis transaction (sends MULTI). */
static int
redis_drv_transaction_begin(csilk_db_pool_t* pool)
{
	return redis_drv_exec(pool, "MULTI");
}

/** @brief Commit the current transaction (sends EXEC). */
static int
redis_drv_transaction_commit(csilk_db_pool_t* pool)
{
	return redis_drv_exec(pool, "EXEC");
}

/** @brief Rollback the current transaction (sends DISCARD). */
static int
redis_drv_transaction_rollback(csilk_db_pool_t* pool)
{
	return redis_drv_exec(pool, "DISCARD");
}

/** @brief Pre-built driver vtable for Redis. */
csilk_db_driver_t csilk_db_redis_driver = {
    .name = "redis",
    .connect = redis_drv_connect,
    .disconnect = redis_drv_disconnect,
    .query = redis_drv_query,
    .exec = redis_drv_exec,
    .transaction_begin = redis_drv_transaction_begin,
    .transaction_commit = redis_drv_transaction_commit,
    .transaction_rollback = redis_drv_transaction_rollback,
    .free_result = redis_free_csilk_result,
};

/**
 * @brief Register the Redis driver with the database subsystem.
 *
 * Makes the "redis" driver available for csilk_db_pool_new().
 * Only compiled when HAS_REDIS is defined (i.e., hiredis was found).
 */
void
csilk_db_redis_init(void)
{
	csilk_db_register_driver("redis", &csilk_db_redis_driver);
}

#endif /* HAS_REDIS */
