/**
 * @file mongodb.c
 * @brief MongoDB database driver for csilk.
 *
 * Implements the csilk_db_driver_t vtable using the native libmongoc C API.
 * Key design points:
 *   - The DSN is a standard MongoDB URI (e.g.,
 * "mongodb://localhost:27017/testdb").
 *   - SELECT queries (via .query) can be:
 *      a) A simple collection name (returns all documents).
 *      b) A JSON command string (e.g., {"find": "users", "filter": {...}}).
 *   - BSON documents are flattened to tabular rows; the first document
 *     defines the column names for the result set.
 *
 * @copyright MIT License
 */

#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/** @brief Per-connection data for the MongoDB driver. */
typedef struct {
	mongoc_client_t* client;
	char* db_name;
} mongodb_conn_t;

/** @brief Initialize libmongoc once. */
static void
mongodb_ensure_init(void)
{
	static int initialized = 0;
	if (__sync_val_compare_and_swap(&initialized, 0, 1) == 0) {
		mongoc_init();
	}
}

/** @brief Open a connection to MongoDB using a URI.
 *
 * Extracts the database name from the URI if provided, otherwise defaults
 * to "test".
 *
 * @param pool The database pool to initialize.
 * @param dsn  MongoDB URI string.
 * @return 0 on success, -1 if URI is invalid or connection fails. */
static int
mongodb_connect(csilk_db_pool_t* pool, const char* dsn)
{
	if (!pool || !dsn) {
		return -1;
	}
	mongodb_ensure_init();

	mongodb_conn_t* conn = calloc(1, sizeof(mongodb_conn_t));
	if (!conn) {
		return -1;
	}

	bson_error_t error;
	mongoc_uri_t* uri = mongoc_uri_new_with_error(dsn, &error);
	if (!uri) {
		CSILK_LOG_E("csilk_db_mongodb: invalid URI '%s': %s", dsn, error.message);
		free(conn);
		return -1;
	}

	const char* db = mongoc_uri_get_database(uri);
	conn->db_name = strdup(db ? db : "test");
	conn->client = mongoc_client_new_from_uri(uri);
	mongoc_uri_destroy(uri);

	if (!conn->client) {
		free(conn->db_name);
		free(conn);
		return -1;
	}

	csilk_db_pool_set_connection(pool, conn);
	return 0;
}

/** @brief Close the MongoDB connection and free associated data. */
static int
mongodb_disconnect(csilk_db_pool_t* pool)
{
	if (!pool || !csilk_db_pool_get_connection(pool)) {
		return -1;
	}
	mongodb_conn_t* conn = (mongodb_conn_t*)csilk_db_pool_get_connection(pool);
	if (conn->client) {
		mongoc_client_destroy(conn->client);
	}
	free(conn->db_name);
	free(conn);
	csilk_db_pool_set_connection(pool, nullptr);
	return 0;
}

/** @brief Free a MongoDB query result set. */
static void
mongodb_free_result(csilk_db_result_t* result)
{
	if (!result) {
		return;
	}
	for (int i = 0; i < result->row_count; i++) {
		for (int j = 0; j < result->column_count; j++) {
			free(result->rows[i]->values[j]);
		}
		free(result->rows[i]->values);
		free(result->rows[i]);
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

/** @brief Helper: Convert a BSON value to a string representation. */
static char*
bson_val_to_str(const bson_iter_t* iter)
{
	bson_type_t type = bson_iter_type(iter);
	switch (type) {
	case BSON_TYPE_UTF8:
		return strdup(bson_iter_utf8(iter, nullptr));
	case BSON_TYPE_INT32: {
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", bson_iter_int32(iter));
		return strdup(buf);
	}
	case BSON_TYPE_INT64: {
		char buf[32];
		snprintf(buf, sizeof(buf), "%ld", (long)bson_iter_int64(iter));
		return strdup(buf);
	}
	case BSON_TYPE_DOUBLE: {
		char buf[32];
		snprintf(buf, sizeof(buf), "%g", bson_iter_double(iter));
		return strdup(buf);
	}
	case BSON_TYPE_BOOL:
		return strdup(bson_iter_bool(iter) ? "true" : "false");
	case BSON_TYPE_OID: {
		char str[25];
		bson_oid_to_string(bson_iter_oid(iter), str);
		return strdup(str);
	}
	case BSON_TYPE_NULL:
		return strdup("");
	default: {
		// For complex types (objects, arrays), stringify the BSON subset
		// This is a simplification.
		return strdup("[complex]");
	}
	}
}

/** @brief Execute a MongoDB query and return results as tabular rows.
 *
 * Support:
 * 1. Simple collection name: "users" -> find all.
 * 2. JSON command: {"find": "users", "filter": {...}} -> execute command.
 */
static int
mongodb_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
		return -1;
	}
	mongodb_conn_t* conn = (mongodb_conn_t*)csilk_db_pool_get_connection(pool);

	bson_error_t error;
	bson_t* cmd = nullptr;
	mongoc_cursor_t* cursor = nullptr;

	if (sql[0] == '{') {
		// Looks like a JSON command
		cmd = bson_new_from_json((const uint8_t*)sql, -1, &error);
		if (!cmd) {
			return -1;
		}
		cursor = mongoc_client_command_with_opts(
		    conn->client, conn->db_name, cmd, nullptr, nullptr, nullptr);
	} else {
		// Treat as collection name
		mongoc_collection_t* coll =
		    mongoc_client_get_collection(conn->client, conn->db_name, sql);
		cursor = mongoc_collection_find_with_opts(coll, bson_new(), nullptr, nullptr);
		mongoc_collection_destroy(coll);
	}

	const bson_t* doc;
	while (mongoc_cursor_next(cursor, &doc)) {
		if (result->column_count == 0) {
			// Use first document to define columns
			bson_iter_t iter;
			if (bson_iter_init(&iter, doc)) {
				while (bson_iter_next(&iter)) {
					result->column_names =
					    realloc(result->column_names,
						    (result->column_count + 1) * sizeof(char*));
					result->column_names[result->column_count++] =
					    strdup(bson_iter_key(&iter));
				}
			}
		}

		csilk_db_row_t* row = calloc(1, sizeof(csilk_db_row_t));
		row->count = result->column_count;
		row->values = calloc(row->count, sizeof(char*));

		for (int i = 0; i < row->count; i++) {
			bson_iter_t iter;
			if (bson_iter_init_find(&iter, doc, result->column_names[i])) {
				row->values[i] = bson_val_to_str(&iter);
			} else {
				row->values[i] = strdup("");
			}
		}

		result->rows =
		    realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
		result->rows[result->row_count++] = row;
	}

	if (mongoc_cursor_error(cursor, &error)) {
		CSILK_LOG_E("csilk_db_mongodb: cursor error: %s", error.message);
		mongoc_cursor_destroy(cursor);
		if (cmd) {
			bson_destroy(cmd);
		}
		mongodb_free_result(result);
		return -1;
	}

	mongoc_cursor_destroy(cursor);
	if (cmd) {
		bson_destroy(cmd);
	}
	return 0;
}

/** @brief Execute a non-query command (INSERT, UPDATE, DELETE).
 *
 * The 'sql' parameter should be a JSON command.
 */
static int
mongodb_exec(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
		return -1;
	}
	mongodb_conn_t* conn = (mongodb_conn_t*)csilk_db_pool_get_connection(pool);

	bson_error_t error;
	bson_t* cmd = bson_new_from_json((const uint8_t*)sql, -1, &error);
	if (!cmd) {
		CSILK_LOG_E("csilk_db_mongodb: invalid command JSON: %s", error.message);
		return -1;
	}

	bson_t reply;
	bool success =
	    mongoc_client_command_simple(conn->client, conn->db_name, cmd, nullptr, &reply, &error);
	if (!success) {
		CSILK_LOG_E("csilk_db_mongodb: exec failed: %s", error.message);
	}

	bson_destroy(cmd);
	bson_destroy(&reply);
	return success ? 0 : -1;
}

static int
mongodb_transaction_begin(csilk_db_pool_t* pool)
{
	(void)pool;
	return 0;
}
static int
mongodb_transaction_commit(csilk_db_pool_t* pool)
{
	(void)pool;
	return 0;
}
static int
mongodb_transaction_rollback(csilk_db_pool_t* pool)
{
	(void)pool;
	return 0;
}

csilk_db_driver_t csilk_db_mongodb_driver = {
    .name = "mongodb",
    .connect = mongodb_connect,
    .disconnect = mongodb_disconnect,
    .query = mongodb_query,
    .exec = mongodb_exec,
    .transaction_begin = mongodb_transaction_begin,
    .transaction_commit = mongodb_transaction_commit,
    .transaction_rollback = mongodb_transaction_rollback,
    .free_result = mongodb_free_result,
};

void
csilk_db_mongodb_init(void)
{
	csilk_db_register_driver("mongodb", &csilk_db_mongodb_driver);
}
