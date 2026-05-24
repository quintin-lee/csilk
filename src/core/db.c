/**
 * @file db.c
 * @brief Unified database interface implementation.
 * @copyright MIT License
 */

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_db.h"

/** @brief Initialize the database system (call once on startup).
 * Registers built-in drivers including SQLite3. */
void csilk_db_init(void) { csilk_db_sqlite_init(); }

/** @brief Registered drivers. */
static csilk_db_driver_t* drivers[16];
static int driver_count = 0;

/** @brief Create a new database pool. */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn) {
  if (!driver_name) return NULL;

  csilk_db_driver_t* driver = csilk_db_get_driver(driver_name);
  if (!driver) {
    fprintf(stderr, "csilk_db: driver '%s' not found\n", driver_name);
    return NULL;
  }

  csilk_db_pool_t* pool = calloc(1, sizeof(csilk_db_pool_t));
  if (!pool) return NULL;

  pool->driver = driver;
  if (driver->connect(pool, dsn) != 0) {
    free(pool);
    return NULL;
  }

  return pool;
}

/** @brief Free a database pool. */
void csilk_db_pool_free(csilk_db_pool_t* pool) {
  if (!pool) return;
  if (pool->driver && pool->driver->disconnect) {
    pool->driver->disconnect(pool);
  }
  free(pool);
}

/** @brief Execute a query and return JSON result. */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->driver || !pool->driver->query) return NULL;

  csilk_db_result_t result = {0};
  if (pool->driver->query(pool, sql, &result) != 0) {
    return NULL;
  }

  cJSON* array = cJSON_CreateArray();
  if (!array) {
    pool->driver->free_result(&result);
    return NULL;
  }

  for (int i = 0; i < result.row_count; i++) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
      cJSON_Delete(array);
      pool->driver->free_result(&result);
      return NULL;
    }

    csilk_db_row_t* row = result.rows[i];
    for (int j = 0; j < row->count; j++) {
      if (row->values[j]) {
        cJSON_AddItemToObject(
            obj, result.column_names ? result.column_names[j] : "col",
            cJSON_CreateString(row->values[j]));
      }
    }
    cJSON_AddItemToArray(array, obj);
  }

  pool->driver->free_result(&result);
  return array;
}

/** @brief Execute a statement. */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->driver || !pool->driver->exec) return -1;
  return pool->driver->exec(pool, sql);
}

/** @brief Query with parameters. */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql,
                                 const char** params) {
  if (!pool || !pool->driver || !pool->driver->query) return NULL;

  // Build query with parameters (simplified - adds quotes for strings)
  size_t sql_len = strlen(sql);
  size_t param_count = 0;
  const char** p = params;
  while (*p) {
    sql_len += strlen(*p) + 2; /* +2 for quotes */
    param_count++;
    p++;
  }

  char* full_sql = malloc(sql_len + 32); /* extra buffer for quotes */
  if (!full_sql) return NULL;

  int pos = 0;
  p = params;
  int param_idx = 0;
  for (size_t i = 0; sql[i]; i++) {
    if (sql[i] == '?' && param_idx < param_count) {
      const char* val = p[param_idx];
      full_sql[pos++] = '\'';
      if (val) {
        size_t len = strlen(val);
        memcpy(full_sql + pos, val, len);
        pos += len;
      }
      full_sql[pos++] = '\'';
      param_idx++;
    } else {
      full_sql[pos++] = sql[i];
    }
  }
  full_sql[pos] = '\0';

  cJSON* result = csilk_db_query_json(pool, full_sql);
  free(full_sql);
  return result;
}

/** @brief Register a driver. */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver) {
  if (!name || !driver || driver_count >= 16) return -1;

  driver->name = name;
  drivers[driver_count++] = driver;
  return 0;
}

/** @brief Get a driver by name. */
csilk_db_driver_t* csilk_db_get_driver(const char* name) {
  if (!name) return NULL;
  for (int i = 0; i < driver_count; i++) {
    if (strcmp(drivers[i]->name, name) == 0) {
      return drivers[i];
    }
  }
  return NULL;
}
