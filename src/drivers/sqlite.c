/**
 * @file sqlite.c
 * @brief SQLite3 database driver for csilk.
 * @copyright MIT License
 */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_db.h"

/** @brief SQLite connection data. */
typedef struct {
  sqlite3* db;
} sqlite_conn_t;

/** @brief Connect to SQLite database. */
static int sqlite_connect(csilk_db_pool_t* pool, const char* dsn) {
  if (!pool || !dsn) return -1;

  sqlite_conn_t* conn = calloc(1, sizeof(sqlite_conn_t));
  if (!conn) return -1;

  int rc = sqlite3_open_v2(dsn, &conn->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "csilk_db_sqlite: cannot open '%s': %s\n", dsn,
            sqlite3_errmsg(conn->db));
    free(conn);
    return -1;
  }

  pool->connection = conn;
  return 0;
}

/** @brief Disconnect from SQLite. */
static int sqlite_disconnect(csilk_db_pool_t* pool) {
  if (!pool || !pool->connection) return -1;

  sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;
  if (conn->db) {
    sqlite3_close(conn->db);
  }
  free(conn);
  pool->connection = NULL;
  return 0;
}

/** @brief Free result set. */
static void sqlite_free_result(csilk_db_result_t* result) {
  if (!result) return;
  for (int i = 0; i < result->row_count; i++) {
    free(result->rows[i]);
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

/** @brief Execute a query and return results. */
static int sqlite_query(csilk_db_pool_t* pool, const char* sql,
                        csilk_db_result_t* result) {
  if (!pool || !pool->connection || !sql || !result) return -1;

  sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;

  sqlite3_stmt* stmt = NULL;
  int rc = sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "csilk_db_sqlite: prepare failed: %s\n",
            sqlite3_errmsg(conn->db));
    return -1;
  }

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

    for (int i = 0; i < result->column_count; i++) {
      const char* val = (const char*)sqlite3_column_text(stmt, i);
      row->values[i] = val ? strdup(val) : strdup("");
    }

    result->rows = realloc(result->rows,
                           (result->row_count + 1) * sizeof(csilk_db_row_t*));
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

/** @brief Execute a statement. */
static int sqlite_exec(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->connection || !sql) return -1;

  sqlite_conn_t* conn = (sqlite_conn_t*)pool->connection;
  char* err = NULL;
  int rc = sqlite3_exec(conn->db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "csilk_db_sqlite: exec failed: %s\n",
            err ? err : "unknown");
    if (err) sqlite3_free(err);
    return -1;
  }
  return 0;
}

/** @brief Begin transaction. */
static int sqlite_transaction_begin(csilk_db_pool_t* pool) {
  return sqlite_exec(pool, "BEGIN TRANSACTION");
}

/** @brief Commit transaction. */
static int sqlite_transaction_commit(csilk_db_pool_t* pool) {
  return sqlite_exec(pool, "COMMIT");
}

/** @brief Rollback transaction. */
static int sqlite_transaction_rollback(csilk_db_pool_t* pool) {
  return sqlite_exec(pool, "ROLLBACK");
}

/** @brief SQLite driver instance. */
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

/** @brief Initialize SQLite driver (register with framework). */
void csilk_db_sqlite_init(void) {
  csilk_db_register_driver("sqlite", &csilk_db_sqlite_driver);
}
