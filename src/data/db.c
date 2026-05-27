/**
 * @file db.c
 * @brief Unified database interface implementation.
 *
 * ## Architecture
 * The DB layer uses a pluggable driver registry pattern:
 *
 *   csilk_db_driver_t (abstract interface)
 *     ├── connect(pool, dsn) → establishes connection
 *     ├── query(pool, sql, result) → returns tabular results
 *     ├── exec(pool, sql) → executes non-query statements
 *     ├── free_result(result) → releases query results
 *     └── disconnect(pool) → tears down connection
 *
 * Drivers self-register at startup (e.g., csilk_db_sqlite_init() calls
 * csilk_db_register_driver()). The registry is a simple fixed-size array
 * protected by a mutex.
 *
 * ## Pool lifecycle
 * csilk_db_pool_new() → driver->connect() → [query/exec] → pool_free() →
 * driver->disconnect()
 *
 * The pool itself is a thin wrapper: it holds a driver pointer, a mutex
 * (for serializing access to the single connection), and driver-specific
 * state in an opaque `handle` field.
 *
 * ## JSON result conversion
 * csilk_db_query_json() and friends convert the driver's tabular result
 * (csilk_db_result_t) into a cJSON array of objects — one object per row,
 * with column names as keys. This is the format expected by the HTTP layer
 * for JSON API responses.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/db.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

/** @brief Mutex protecting the driver registry during registration/lookup. */
static uv_mutex_t registry_mutex;
static int registry_initialized = 0;

/** @brief Initialize the database subsystem (call once at process startup).
 *
 * ## Double-checked locking pattern
 * Uses GCC __sync_val_compare_and_swap (atomic CAS) for thread-safe
 * single initialization without a full mutex on the fast path:
 *   1. Atomic CAS sets registry_initialized from 0→1. Only the winning
 *      thread enters the body.
 *   2. Inside: initializes the registry mutex.
 *   3. Registers built-in drivers: SQLite3 always; MySQL and PostgreSQL
 *      only if compiled with HAS_MYSQL / HAS_POSTGRES.
 *
 * @note Must be called before any csilk_db_pool_new() or driver registration
 *       calls. Safe to call from multiple threads concurrently. */
void csilk_db_init(void) {
  if (__sync_val_compare_and_swap(&registry_initialized, 0, 1) == 0) {
    uv_mutex_init(&registry_mutex);
  }
  csilk_db_sqlite_init();
#ifdef HAS_MYSQL
  csilk_db_mysql_init();
#endif
#ifdef HAS_POSTGRES
  csilk_db_postgres_init();
#endif
}

/** @brief Statically-sized registry of registered database drivers (max 16). */
static csilk_db_driver_t* drivers[16];
static int driver_count = 0;

/** @brief Internal: execute a query and return the result as a cJSON array.
 *
 * ## Row-to-JSON conversion
 *   1. Call driver->query() to get the raw tabular result (rows + columns).
 *   2. Create an empty cJSON array.
 *   3. For each row (result.rows[i]):
 *      a. Create a cJSON object.
 *      b. For each field (row->values[j]):
 *         - Add (column_name[j], field_value) as a string key-value pair.
 *         - If column_names is NULL, use "col" as the key.
 *         - NULL values are skipped (not added to the object).
 *      c. Append the object to the array.
 *   4. Call driver->free_result() to release the driver's memory.
 *   5. Return the cJSON array (caller must cJSON_Delete()).
 *
 * All values are represented as cJSON strings — no type inference is
 * attempted. The caller can parse numerics/bools with cJSON_GetNumberValue
 * etc. if needed.
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

/** @brief Create a new database connection pool with the named driver.
 *
 * ## Pool creation
 *   1. Look up the driver by name in the global registry.
 *   2. calloc the pool struct, init the pool mutex.
 *   3. Call driver->connect() with the DSN — this may block for network
 *      round-trips.
 *   4. On success: return the pool. On failure: destroy mutex, free pool.
 *
 * The pool holds exactly ONE connection (mutex-serialized access). This is
 * intentional — the caller is expected to manage a pool of pools if
 * concurrency is needed.
 *
 * @param driver_name Registered driver name (e.g., "sqlite3").
 * @param dsn         Data source name (e.g., "/tmp/test.db" or "host=..."). */
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

/** @brief Free a database pool and its underlying connection.
 *
 * ## Teardown
 *   1. Call driver->disconnect() — closes the database connection.
 *   2. Destroy the pool mutex.
 *   3. Free the pool struct.
 *
 * The driver's free_result is not called here — any outstanding results
 * must have been freed by the caller. */
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
 * ## String substitution algorithm
 *   1. Pre-compute the final SQL length: original SQL + sum of all param
 *      lengths + 2 bytes per param for surrounding single quotes.
 *   2. Allocate a buffer of that size.
 *   3. Walk the original SQL character by character:
 *      - '?' → replace with 'value' (single-quote wrapped)
 *      - any other char → copy verbatim.
 *   4. Execute the constructed SQL via csilk_db_query_json_locked().
 *
 * ## Security caveat
 * This is naive string substitution — NOT prepared-statement binding.
 * Parameter values are NOT escaped for SQL special characters. A parameter
 * containing "' OR '1'='1" will inject into the SQL verbatim.
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

/** @brief Register a database driver in the global registry.
 *
 * Appends the driver to the fixed-size drivers[] array (max 16).
 * The driver's `name` field is set to the provided `name` string.
 * The `name` pointer must remain valid for the lifetime of the driver
 * (typically a static string literal).
 *
 * @param name   Driver name for lookup (e.g., "sqlite3").
 * @param driver Driver vtable with connect/query/exec/free_result/disconnect.
 * @return 0 on success, -1 if the registry is full or parameters are NULL. */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver) {
  if (!name || !driver || driver_count >= 16) return -1;

  uv_mutex_lock(&registry_mutex);
  driver->name = name;
  drivers[driver_count++] = driver;
  uv_mutex_unlock(&registry_mutex);
  return 0;
}

/** @brief Look up a registered database driver by name.
 *
 * Linear scan of the drivers[] array under the registry mutex.
 * Returns the first driver whose name matches (strcmp).
 *
 * @param name Driver name to look up (e.g., "sqlite3").
 * @return The registered driver, or NULL if not found. */
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
