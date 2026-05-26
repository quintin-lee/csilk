#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_db.h"

typedef struct {
  MYSQL* db;
} mysql_conn_t;

static void mysql_parse_dsn(const char* dsn, char** host, int* port,
                            char** user, char** password, char** dbname) {
  *host = NULL;
  *port = 3306;
  *user = NULL;
  *password = NULL;
  *dbname = NULL;
  if (!dsn) return;

  char* buf = strdup(dsn);
  if (!buf) return;

  char* token = strtok(buf, ";");
  while (token) {
    while (*token == ' ') token++;
    char* eq = strchr(token, '=');
    if (!eq) { token = strtok(NULL, ";"); continue; }
    *eq = '\0';
    const char* key = token;
    const char* val = eq + 1;

    if (strcmp(key, "host") == 0) *host = strdup(val);
    else if (strcmp(key, "port") == 0) *port = atoi(val);
    else if (strcmp(key, "user") == 0) *user = strdup(val);
    else if (strcmp(key, "password") == 0) *password = strdup(val);
    else if (strcmp(key, "dbname") == 0) *dbname = strdup(val);

    token = strtok(NULL, ";");
  }
  free(buf);
}

static int mysql_drv_connect(csilk_db_pool_t* pool, const char* dsn) {
  if (!pool || !dsn) return -1;

  mysql_conn_t* conn = calloc(1, sizeof(mysql_conn_t));
  if (!conn) return -1;

  conn->db = mysql_init(NULL);
  if (!conn->db) {
    fprintf(stderr, "csilk_db_mysql: mysql_init failed\n");
    free(conn);
    return -1;
  }

  char *host = NULL, *user = NULL, *password = NULL, *dbname = NULL;
  int port = 3306;
  mysql_parse_dsn(dsn, &host, &port, &user, &password, &dbname);

  MYSQL* ret = mysql_real_connect(conn->db, host, user, password, dbname,
                                  port, NULL, 0);
  free(host);
  free(user);
  free(password);
  free(dbname);

  if (!ret) {
    fprintf(stderr, "csilk_db_mysql: connect failed: %s\n",
            mysql_error(conn->db));
    mysql_close(conn->db);
    free(conn);
    return -1;
  }

  pool->connection = conn;
  return 0;
}

static int mysql_drv_disconnect(csilk_db_pool_t* pool) {
  if (!pool || !pool->connection) return -1;

  mysql_conn_t* conn = (mysql_conn_t*)pool->connection;
  if (conn->db) mysql_close(conn->db);
  free(conn);
  pool->connection = NULL;
  return 0;
}

static void mysql_free_csilk_result(csilk_db_result_t* result) {
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

static int mysql_drv_query(csilk_db_pool_t* pool, const char* sql,
                       csilk_db_result_t* result) {
  if (!pool || !pool->connection || !sql || !result) return -1;

  mysql_conn_t* conn = (mysql_conn_t*)pool->connection;

  if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
    fprintf(stderr, "csilk_db_mysql: query failed: %s\n",
            mysql_error(conn->db));
    return -1;
  }

  MYSQL_RES* mysql_res = mysql_store_result(conn->db);
  if (!mysql_res) {
    return 0;
  }

  MYSQL_FIELD* fields = mysql_fetch_fields(mysql_res);
  unsigned int num_fields = mysql_num_fields(mysql_res);

  result->column_count = (int)num_fields;
  result->column_names = calloc(num_fields, sizeof(char*));
  if (!result->column_names) { mysql_free_result(mysql_res); return -1; }
  for (unsigned int i = 0; i < num_fields; i++) {
    result->column_names[i] = strdup(fields[i].name);
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(mysql_res))) {
    unsigned long* lengths = mysql_fetch_lengths(mysql_res);

    csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
    if (!drow) { mysql_free_result(mysql_res); mysql_free_csilk_result(result); return -1; }

    drow->count = (int)num_fields;
    drow->values = calloc(num_fields, sizeof(char*));
    if (!drow->values) { free(drow); mysql_free_result(mysql_res); mysql_free_csilk_result(result); return -1; }

    for (unsigned int i = 0; i < num_fields; i++) {
      drow->values[i] = row[i] ? strndup(row[i], lengths[i]) : NULL;
    }

    csilk_db_row_t** new_rows = realloc(
        result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
    if (!new_rows) { free(drow->values); free(drow); mysql_free_result(mysql_res); mysql_free_csilk_result(result); return -1; }
    result->rows = new_rows;
    result->rows[result->row_count++] = drow;
  }

  mysql_free_result(mysql_res);
  return 0;
}

static int mysql_drv_exec(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->connection || !sql) return -1;

  mysql_conn_t* conn = (mysql_conn_t*)pool->connection;

  if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
    fprintf(stderr, "csilk_db_mysql: exec failed: %s\n",
            mysql_error(conn->db));
    return -1;
  }

  MYSQL_RES* res = mysql_store_result(conn->db);
  if (res) mysql_free_result(res);

  return 0;
}

static int mysql_drv_transaction_begin(csilk_db_pool_t* pool) {
  return mysql_drv_exec(pool, "BEGIN");
}

static int mysql_drv_transaction_commit(csilk_db_pool_t* pool) {
  return mysql_drv_exec(pool, "COMMIT");
}

static int mysql_drv_transaction_rollback(csilk_db_pool_t* pool) {
  return mysql_drv_exec(pool, "ROLLBACK");
}

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

void csilk_db_mysql_init(void) {
  csilk_db_register_driver("mysql", &csilk_db_mysql_driver);
}
