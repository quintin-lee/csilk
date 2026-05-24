/**
 * @file csilk_db.h
 * @brief Unified database interface for csilk web framework.
 * @copyright MIT License
 */

#ifndef CSILK_DB_H
#define CSILK_DB_H

#include <stddef.h>
#include <stdint.h>

#include "csilk.h"

/** @brief Database driver interface. */
typedef struct csilk_db_driver_s csilk_db_driver_t;

/** @brief Database connection pool. */
typedef struct {
  csilk_db_driver_t* driver;
  void* connection;
  uv_mutex_t mutex;
} csilk_db_pool_t;

/** @brief Query result row. */
typedef struct {
  char** values;
  int count;
} csilk_db_row_t;

/** @brief Query result set. */
typedef struct {
  csilk_db_row_t** rows;
  char** column_names;
  int row_count;
  int column_count;
} csilk_db_result_t;

/** @brief Driver function pointers. */
struct csilk_db_driver_s {
  const char* name;

  /** @brief Connect to database.
   * @param pool Connection pool.
   * @param dsn Data source name (e.g., "file:test.db" or
   * "host=localhost;user=foo").
   * @return 0 on success, -1 on failure. */
  int (*connect)(csilk_db_pool_t* pool, const char* dsn);

  /** @brief Disconnect from database. */
  int (*disconnect)(csilk_db_pool_t* pool);

  /** @brief Execute a query and return results.
   * @param pool Connection pool.
   * @param sql SQL statement.
   * @param result Output result set.
   * @return 0 on success, -1 on failure. */
  int (*query)(csilk_db_pool_t* pool, const char* sql,
               csilk_db_result_t* result);

  /** @brief Execute a statement (no result rows).
   * @param pool Connection pool.
   * @param sql SQL statement.
   * @return 0 on success, -1 on failure. */
  int (*exec)(csilk_db_pool_t* pool, const char* sql);

  /** @brief Begin transaction. */
  int (*transaction_begin)(csilk_db_pool_t* pool);

  /** @brief Commit transaction. */
  int (*transaction_commit)(csilk_db_pool_t* pool);

  /** @brief Rollback transaction. */
  int (*transaction_rollback)(csilk_db_pool_t* pool);

  /** @brief Free result set. */
  void (*free_result)(csilk_db_result_t* result);
};

/** @brief Create a new database pool with the given driver.
 * @param driver_name Driver name (e.g., "sqlite").
 * @param dsn Data source name.
 * @return New pool, or NULL on failure. */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/** @brief Free a database pool. */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/** @brief Execute a query and return JSON result.
 * @param pool Connection pool.
 * @param sql SQL statement.
 * @return cJSON array, or NULL on failure. Caller must free. */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/** @brief Execute a statement.
 * @param pool Connection pool.
 * @param sql SQL statement.
 * @return 0 on success, -1 on failure. */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/** @brief Query with parameters (placeholder: ?).
 * @param pool Connection pool.
 * @param sql SQL with ? placeholders.
 * @param params Parameter values (NULL-terminated).
 * @return cJSON array, or NULL on failure. */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql,
                                 const char** params);

/** @brief Register a database driver.
 * @param name Driver identifier.
 * @param driver Driver implementation.
 * @return 0 on success. */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver);

/** @brief Get a registered driver by name.
 * @param name Driver identifier.
 * @return Driver pointer, or NULL if not found. */
csilk_db_driver_t* csilk_db_get_driver(const char* name);

/** @brief Initialize SQLite driver (internal use). */
void csilk_db_sqlite_init(void);

/** @brief Initialize the database system (call once on startup).
 * Registers built-in drivers including SQLite3. */
void csilk_db_init(void);

#endif /* CSILK_DB_H */
