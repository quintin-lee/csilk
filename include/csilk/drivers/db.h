/**
 * @file csilk_db.h
 * @brief Unified pluggable database interface for the csilk web framework.
 *
 * Provides an abstract driver model (similar to Go's database/sql) so that
 * different backends (SQLite, PostgreSQL, MySQL, etc.) can be used through a
 * common API.  All public database functions are declared here; driver
 * implementations live in separate compilation units.
 *
 * @copyright MIT License
 */

#ifndef CSILK_DB_H
#define CSILK_DB_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/csilk.h"

/**
 * @brief Database statistics.
 */
typedef struct {
  uint64_t queries_total;     /**< Total SELECT queries executed. */
  uint64_t execs_total;       /**< Total non-query statements (INSERT/UPDATE/etc). */
  uint64_t errors_total;      /**< Total failed operations. */
  uint64_t duration_us_total; /**< Cumulative duration in microseconds. */
} csilk_db_stats_t;

/**
 * @brief Get current database statistics.
 * @param stats [out] Pointer to stats struct to populate.
 */
void csilk_db_get_stats(csilk_db_stats_t* stats);

/**
 * @brief Opaque database driver interface.
 *
 * Each supported database backend implements the function table in
 * csilk_db_driver_s and registers itself with csilk_db_register_driver.
 */
typedef struct csilk_db_driver_s csilk_db_driver_t;

/**
 * @brief A (currently single-connection) database pool.
 *
 * Wraps a driver + connection with a mutex for thread-safe access.
 * All queries and statements go through this pool.  The pool is created
 * via csilk_db_pool_new (which opens the connection) and destroyed via
 * csilk_db_pool_free (which closes it).
 *
 * ## Thread Safety
 * The pool's mutex serialises all driver operations across libuv worker
 * threads (uv_queue_work).  Functions like csilk_db_query_json acquire
 * the mutex automatically.  Do NOT call driver functions directly without
 * holding the mutex.
 *
 * @note The current implementation holds exactly one connection.  A future
 *       version may support a true connection pool with multiple replicas.
 */
typedef struct csilk_db_pool_s {
  csilk_db_driver_t* driver; /**< Pointer to the registered driver
                                implementation (set by csilk_db_pool_new). */
  void* connection; /**< Opaque driver-specific connection handle (set by
                       the driver's connect callback). */
  uv_mutex_t mutex; /**< Mutex serialising access to @p connection across
                       threads.  Held automatically by pool-level functions. */
} csilk_db_pool_t;

/**
 * @brief A single row of a query result.
 *
 * Values are strings (human-readable representations of column data).
 * NULL database values are represented as NULL pointers in the values array.
 */
typedef struct {
  char** values; /**< Array of NUL-terminated string values, one per column.
                    NULL if the SQL value was NULL. */
  int count;     /**< Number of columns (length of @p values). */
} csilk_db_row_t;

/**
 * @brief A complete query result set.
 *
 * Contains all rows, column names, and dimension information.
 * Allocated by the driver's query function and freed by free_result.
 */
typedef struct {
  csilk_db_row_t** rows; /**< Array of row pointers (length @p row_count). */
  char** column_names;   /**< Array of column name strings (length @p
                            column_count). */
  int row_count;         /**< Number of rows in @p rows. */
  int column_count; /**< Number of columns (length of @p column_names and each
                       row's @p values). */
} csilk_db_result_t;

/**
 * @brief Virtual function table implemented by each database driver.
 *
 * All function pointers must be non-NULL except where noted.
 */
struct csilk_db_driver_s {
  const char* name; /**< Driver identifier string (e.g., "sqlite", "postgres").
                       Matches the name passed to csilk_db_pool_new. */

  /** @brief Open a connection to the database.
   *  @param pool  Pool whose @p connection field should be populated.
   *  @param dsn   Driver-specific data source name.
   *  @return 0 on success, -1 on failure. */
  int (*connect)(csilk_db_pool_t* pool, const char* dsn);

  /** @brief Close the database connection.
   *  @param pool  Pool whose @p connection should be closed and freed.
   *  @return 0 on success, -1 on failure. */
  int (*disconnect)(csilk_db_pool_t* pool);

  /** @brief Execute a SELECT query and produce a result set.
   *  @param pool   Connection pool (mutex is held by the caller).
   *  @param sql    SQL query string.
   *  @param[out] result  Caller-allocated csilk_db_result_t to populate.
   *                      Set to zero-initialised (the driver allocates rows +
   *                      strings internally).
   *  @return 0 on success, -1 on failure. */
  int (*query)(csilk_db_pool_t* pool, const char* sql,
               csilk_db_result_t* result);

  /** @brief Execute a statement that returns no result rows.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @param sql   SQL statement (INSERT, UPDATE, DELETE, DDL, etc.).
   *  @return 0 on success, -1 on failure. */
  int (*exec)(csilk_db_pool_t* pool, const char* sql);

  /** @brief Begin a database transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
  int (*transaction_begin)(csilk_db_pool_t* pool);

  /** @brief Commit the current transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
  int (*transaction_commit)(csilk_db_pool_t* pool);

  /** @brief Roll back the current transaction.
   *  @param pool  Connection pool (mutex is held by the caller).
   *  @return 0 on success, -1 on failure. */
  int (*transaction_rollback)(csilk_db_pool_t* pool);

  /** @brief Free a result set and all associated memory.
   *  @param result  Result set to free (must not be NULL). */
  void (*free_result)(csilk_db_result_t* result);
};

/**
 * @brief Create a new database pool and connect using the named driver.
 *
 * Looks up the driver by @p driver_name (must have been registered via
 * csilk_db_register_driver or the built-in init), allocates a pool, and
 * calls the driver's connect function.
 *
 * @param driver_name  Driver identifier (e.g., "sqlite").  Must not be NULL.
 * @param dsn          Data source name (driver-specific format).
 * @return A new csilk_db_pool_t on success, or NULL if the driver is unknown
 *         or the connection fails.
 */
csilk_db_pool_t* csilk_db_pool_new(const char* driver_name, const char* dsn);

/**
 * @brief Destroy a database pool and disconnect.
 *
 * Calls the driver's disconnect, frees internal resources, and deallocates
 * the pool struct.
 *
 * @param pool  Pool to destroy.  Must not be NULL.
 */
void csilk_db_pool_free(csilk_db_pool_t* pool);

/**
 * @brief Execute a SELECT query and return the rows as a JSON array.
 *
 * Internally acquires the pool mutex, calls the driver's query function,
 * converts each row to a JSON object (keyed by column name), frees the
 * native result, and returns the cJSON array.
 *
 * @param pool  Connection pool.
 * @param sql   SQL SELECT statement.
 * @return A cJSON array of row objects, or NULL on failure.  Caller must
 *         free with cJSON_Delete.
 */
cJSON* csilk_db_query_json(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a statement that produces no result rows.
 *
 * Acquires the pool mutex and calls the driver's exec function.
 *
 * @param pool  Connection pool.
 * @param sql   SQL statement (INSERT, UPDATE, DELETE, DDL).
 * @return 0 on success, -1 on failure.
 */
int csilk_db_exec(csilk_db_pool_t* pool, const char* sql);

/**
 * @brief Execute a parameterised SELECT query with ? placeholders.
 *
 * Each ? in @p sql is substituted with the corresponding value from
 * @p params (escaping is handled by the driver).  Results are returned as
 * a JSON array.
 *
 * @param pool   Connection pool.
 * @param sql    SQL with ? placeholders.
 * @param params NULL-terminated array of string values.  The number of
 *               values must match the number of ? placeholders.
 * @return A cJSON array of row objects, or NULL on failure.  Caller must
 *         free with cJSON_Delete.
 */
cJSON* csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql,
                                 const char** params);

/**
 * @brief Register a database driver implementation.
 *
 * Makes the driver available for use with csilk_db_pool_new.  The @p name
 * must be unique (earlier registrations are not overwritten).
 *
 * @param name    Driver identifier string (e.g., "postgres").  Must remain
 *                valid for the program lifetime.
 * @param driver  Pointer to a csilk_db_driver_t with all function pointers
 *                populated.  The struct must remain valid for the program
 *                lifetime.
 * @return 0 on success, -1 if a driver with @p name is already registered.
 */
int csilk_db_register_driver(const char* name, csilk_db_driver_t* driver);

/**
 * @brief Look up a registered driver by name.
 *
 * @param name  Driver identifier string.
 * @return Pointer to the registered csilk_db_driver_t, or NULL if not found.
 */
csilk_db_driver_t* csilk_db_get_driver(const char* name);

/**
 * @brief Internal: Register the built-in SQLite3 driver.
 *
 * Called automatically by csilk_db_init.  Not intended for direct use.
 */
void csilk_db_sqlite_init(void);

/**
 * @brief Internal: Register the built-in MySQL driver.
 *
 * Called automatically by csilk_db_init.  Requires libmysqlclient.
 * Not intended for direct use.
 */
#ifdef HAS_MYSQL
void csilk_db_mysql_init(void);
#endif

/**
 * @brief Internal: Register the built-in PostgreSQL driver.
 *
 * Called automatically by csilk_db_init.  Requires libpq.
 * Not intended for direct use.
 */
#ifdef HAS_POSTGRES
void csilk_db_postgres_init(void);
#endif

/**
 * @brief Internal: Register the built-in MongoDB driver.
 *
 * Called automatically by csilk_db_init. Requires libmongoc.
 * Not intended for direct use.
 */
#ifdef HAS_MONGODB
void csilk_db_mongodb_init(void);
#endif

/**
 * @brief Initialise the database subsystem.
 *
 * Registers all built-in drivers (SQLite3, MySQL, PostgreSQL, etc.).
 * Must be called once before any csilk_db_pool_new call.
 * Safe to call multiple times.
 */
void csilk_db_init(void);

#endif /* CSILK_DB_H */
