#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

typedef struct {
  PGconn* db;
} pg_conn_t;

static int postgres_connect(csilk_db_pool_t* pool, const char* dsn) {
  if (!pool || !dsn) return -1;

  pg_conn_t* conn = calloc(1, sizeof(pg_conn_t));
  if (!conn) return -1;

  conn->db = PQconnectdb(dsn);
  if (!conn->db) {
    fprintf(stderr, "csilk_db_postgres: PQconnectdb failed (OOM)\n");
    free(conn);
    return -1;
  }

  if (PQstatus(conn->db) != CONNECTION_OK) {
    fprintf(stderr, "csilk_db_postgres: connect failed: %s\n",
            PQerrorMessage(conn->db));
    PQfinish(conn->db);
    free(conn);
    return -1;
  }

  pool->connection = conn;
  return 0;
}

static int postgres_disconnect(csilk_db_pool_t* pool) {
  if (!pool || !pool->connection) return -1;

  pg_conn_t* conn = (pg_conn_t*)pool->connection;
  if (conn->db) PQfinish(conn->db);
  free(conn);
  pool->connection = NULL;
  return 0;
}

static void postgres_free_result(csilk_db_result_t* result) {
  if (!result) return;
  for (int i = 0; i < result->row_count; i++) {
    csilk_db_row_t* row = result->rows[i];
    if (row) {
      for (int j = 0; j < row->count; j++) free(row->values[j]);
      free(row->values);
      free(row);
    }
  }
  free(result->rows);
  for (int i = 0; i < result->column_count; i++) free(result->column_names[i]);
  free(result->column_names);
  result->rows = NULL;
  result->column_names = NULL;
  result->row_count = 0;
  result->column_count = 0;
}

static int postgres_query(csilk_db_pool_t* pool, const char* sql,
                          csilk_db_result_t* result) {
  if (!pool || !pool->connection || !sql || !result) return -1;

  pg_conn_t* conn = (pg_conn_t*)pool->connection;

  PGresult* pg_res = PQexec(conn->db, sql);
  if (!pg_res) return -1;

  ExecStatusType status = PQresultStatus(pg_res);
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    fprintf(stderr, "csilk_db_postgres: query failed: %s\n",
            PQresultErrorMessage(pg_res));
    PQclear(pg_res);
    return -1;
  }

  if (status == PGRES_COMMAND_OK) {
    PQclear(pg_res);
    return 0;
  }

  int ntuples = PQntuples(pg_res);
  int nfields = PQnfields(pg_res);

  result->column_count = nfields;
  result->column_names = calloc(nfields, sizeof(char*));
  if (!result->column_names && nfields > 0) { PQclear(pg_res); return -1; }
  for (int i = 0; i < nfields; i++) {
    result->column_names[i] = strdup(PQfname(pg_res, i));
  }

  for (int i = 0; i < ntuples; i++) {
    csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
    if (!drow) { PQclear(pg_res); postgres_free_result(result); return -1; }

    drow->count = nfields;
    drow->values = calloc(nfields, sizeof(char*));
    if (!drow->values) { free(drow); PQclear(pg_res); postgres_free_result(result); return -1; }

    for (int j = 0; j < nfields; j++) {
      drow->values[j] = PQgetisnull(pg_res, i, j) ? NULL : strdup(PQgetvalue(pg_res, i, j));
    }

    csilk_db_row_t** new_rows = realloc(
        result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
    if (!new_rows) { free(drow->values); free(drow); PQclear(pg_res); postgres_free_result(result); return -1; }
    result->rows = new_rows;
    result->rows[result->row_count++] = drow;
  }

  PQclear(pg_res);
  return 0;
}

static int postgres_exec(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->connection || !sql) return -1;

  pg_conn_t* conn = (pg_conn_t*)pool->connection;

  PGresult* pg_res = PQexec(conn->db, sql);
  if (!pg_res) return -1;

  ExecStatusType status = PQresultStatus(pg_res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    fprintf(stderr, "csilk_db_postgres: exec failed: %s\n",
            PQresultErrorMessage(pg_res));
    PQclear(pg_res);
    return -1;
  }

  PQclear(pg_res);
  return 0;
}

static int postgres_transaction_begin(csilk_db_pool_t* pool) {
  return postgres_exec(pool, "BEGIN");
}

static int postgres_transaction_commit(csilk_db_pool_t* pool) {
  return postgres_exec(pool, "COMMIT");
}

static int postgres_transaction_rollback(csilk_db_pool_t* pool) {
  return postgres_exec(pool, "ROLLBACK");
}

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

void csilk_db_postgres_init(void) {
  csilk_db_register_driver("postgres", &csilk_db_postgres_driver);
}
