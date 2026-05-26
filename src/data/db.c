/**
 * @file db.c
 * @brief Unified database interface implementation.
 * @copyright MIT License
 */

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/** @brief Mutex protecting the driver registry during registration/lookup. */
static uv_mutex_t registry_mutex;
static int registry_initialized = 0;

/** @brief Initialize the database subsystem (call once at process startup).
 *
 * Initializes the driver registry mutex and registers all built-in database
 * drivers (currently SQLite3 via csilk_db_sqlite_init()). Thread-safe and
 * idempotent — subsequent calls are no-ops.
 *
 * @note Must be called before any csilk_db_pool_new() or driver registration
 *       calls. Safe to call from multiple threads concurrently (uses atomic
 *       compare-and-swap for single initialization). */
void csilk_db_init(void) {
  if (__sync_val_compare_and_swap(&registry_initialized, 0, 1) == 0) {
    uv_mutex_init(&registry_mutex);
  }
  csilk_db_sqlite_init();
  csilk_db_mysql_init();
  csilk_db_postgres_init();
}

/** @brief Statically-sized registry of registered database drivers (max 16). */
static csilk_db_driver_t* drivers[16];
static int driver_count = 0;

/** @brief Internal: execute a query and return the result as a cJSON array.
 *
 * Calls the driver's query() method and converts the tabular result into a
 * cJSON array of objects (one object per row, field names as keys).
 * The caller must hold the pool mutex before calling this function.
 *
 * @param pool Database pool with an active connection.
 * @param sql  SQL query string.
 * @return A cJSON array of row objects, or NULL on failure.
 * @note The returned cJSON must be freed by the caller with cJSON_Delete().
 * @warning The pool mutex must be held for the duration of this call. */
static cJSON* csilk_db_query_json_locked(csilk_db_pool_t* pool,
                                         const char* sql) {
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

/** @brief Create a new database connection pool with the named driver. */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn) {
  if (!driver_name) return NULL;

  csilk_db_driver_t* driver = csilk_db_get_driver(driver_name);
  if (!driver) {
    fprintf(stderr, "csilk_db: driver '%s' not found\n", driver_name);
    return NULL;
  }

  csilk_db_pool_t* pool = calloc(1, sizeof(csilk_db_pool_t));
  if (!pool) return NULL;

  uv_mutex_init(&pool->mutex);
  pool->driver = driver;
  if (driver->connect(pool, dsn) != 0) {
    uv_mutex_destroy(&pool->mutex);
    free(pool);
    return NULL;
  }

  return pool;
}

/** @brief Free a database pool and its underlying connection. */
void csilk_db_pool_free(csilk_db_pool_t* pool) {
  if (!pool) return;
  if (pool->driver && pool->driver->disconnect) {
    pool->driver->disconnect(pool);
  }
  uv_mutex_destroy(&pool->mutex);
  free(pool);
}

/** @brief Execute a SQL query and return the result as a cJSON array. */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->driver || !pool->driver->query) return NULL;

  uv_mutex_lock(&pool->mutex);
  cJSON* result = csilk_db_query_json_locked(pool, sql);
  uv_mutex_unlock(&pool->mutex);
  return result;
}

/** @brief Execute a SQL statement (INSERT, UPDATE, DELETE, DDL) that does not
 * return rows. */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql) {
  if (!pool || !pool->driver || !pool->driver->exec) return -1;
  uv_mutex_lock(&pool->mutex);
  int rc = pool->driver->exec(pool, sql);
  uv_mutex_unlock(&pool->mutex);
  return rc;
}

/** @brief Execute a parameterized SQL query and return JSON result.
 *
 * Replaces '?' placeholders in the SQL string with the provided parameter
 * values (each wrapped in single quotes for string escaping). This is a
 * simple string substitution — NOT prepared-statement binding — so it is
 * vulnerable to SQL injection if parameter values are untrusted.
 *
 * @param pool   Database pool.
 * @param sql    SQL query with '?' placeholders.
 * @param params NULL-terminated array of string parameter values.
 * @return A cJSON array of row objects, or NULL on failure.
 * @note Thread-safe (mutex-protected).
 * @warning This function does NOT properly escape parameter values. For
 *          production use with untrusted input, use the driver's native
 *          prepared statement API instead. */
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

  uv_mutex_lock(&pool->mutex);
  cJSON* result = csilk_db_query_json_locked(pool, full_sql);
  uv_mutex_unlock(&pool->mutex);
  free(full_sql);
  return result;
}

/** @brief Register a database driver in the global registry. */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver) {
  if (!name || !driver || driver_count >= 16) return -1;

  uv_mutex_lock(&registry_mutex);
  driver->name = name;
  drivers[driver_count++] = driver;
  uv_mutex_unlock(&registry_mutex);
  return 0;
}

/** @brief Look up a registered database driver by name. */
csilk_db_driver_t* csilk_db_get_driver(const char* name) {
  if (!name) return NULL;
  uv_mutex_lock(&registry_mutex);
  for (int i = 0; i < driver_count; i++) {
    if (strcmp(drivers[i]->name, name) == 0) {
      uv_mutex_unlock(&registry_mutex);
      return drivers[i];
    }
  }
  uv_mutex_unlock(&registry_mutex);
  return NULL;
}
