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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "cJSON.h"

/* --- Global DB Metrics --- */
static atomic_uint_fast64_t db_queries_total = 0;
static atomic_uint_fast64_t db_execs_total = 0;
static atomic_uint_fast64_t db_errors_total = 0;
static atomic_uint_fast64_t db_duration_us_total = 0;

void
csilk_db_get_stats(csilk_db_stats_t* stats)
{
	if (!stats) {
		return;
	}
	stats->queries_total = atomic_load(&db_queries_total);
	stats->execs_total = atomic_load(&db_execs_total);
	stats->errors_total = atomic_load(&db_errors_total);
	stats->duration_us_total = atomic_load(&db_duration_us_total);
}

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
static cJSON*
csilk_db_query_json_locked(csilk_db_pool_t* pool, const char* sql)
{
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
				cJSON_AddItemToObject(obj,
						      result.column_names ? result.column_names[j]
									  : "col",
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
csilk_db_pool_t*
csilk_db_pool_new(const char* driver_name, const char* dsn)
{
	if (!driver_name) {
		return NULL;
	}

	csilk_db_driver_t* driver = csilk_db_get_driver(driver_name);
	if (!driver) {
		fprintf(stderr, "csilk_db: driver '%s' not found\n", driver_name);
		return NULL;
	}

	csilk_db_pool_t* pool = calloc(1, sizeof(csilk_db_pool_t));
	if (!pool) {
		return NULL;
	}

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
void
csilk_db_pool_free(csilk_db_pool_t* pool)
{
	if (!pool) {
		return;
	}
	if (pool->driver && pool->driver->disconnect) {
		pool->driver->disconnect(pool);
	}
	uv_mutex_destroy(&pool->mutex);
	free(pool);
}

/** @brief Execute a SQL query and return the result as a cJSON array. */
cJSON*
csilk_db_query_json(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !pool->driver || !pool->driver->query) {
		return NULL;
	}

	uint64_t start = uv_hrtime();
	uv_mutex_lock(&pool->mutex);
	cJSON* result = csilk_db_query_json_locked(pool, sql);
	uv_mutex_unlock(&pool->mutex);

	uint64_t duration = (uv_hrtime() - start) / 1000;
	atomic_fetch_add(&db_duration_us_total, duration);

	if (!result) {
		atomic_fetch_add(&db_errors_total, 1);
	} else {
		atomic_fetch_add(&db_queries_total, 1);
	}
	return result;
}

/** @brief Execute a SQL statement (INSERT, UPDATE, DELETE, DDL) that does not
 * return rows. */
int
csilk_db_exec(csilk_db_pool_t* pool, const char* sql)
{
	if (!pool || !pool->driver || !pool->driver->exec) {
		return -1;
	}

	uint64_t start = uv_hrtime();
	uv_mutex_lock(&pool->mutex);
	int rc = pool->driver->exec(pool, sql);
	uv_mutex_unlock(&pool->mutex);

	uint64_t duration = (uv_hrtime() - start) / 1000;
	atomic_fetch_add(&db_duration_us_total, duration);

	if (rc != 0) {
		atomic_fetch_add(&db_errors_total, 1);
	} else {
		atomic_fetch_add(&db_execs_total, 1);
	}
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
 * @param sql    SQL pattern with ? placeholders.
 * @param params NULL-terminated array of string values. */
cJSON*
csilk_db_query_param_json(csilk_db_pool_t* pool, const char* sql, const char** params)
{
	if (!pool || !sql || !params) {
		return NULL;
	}

	uint64_t start = uv_hrtime();

	size_t len = strlen(sql);
	for (int i = 0; params[i]; i++) {
		len += strlen(params[i]) + 2; /* +2 for quotes */
	}

	char* full_sql = malloc(len + 1);
	if (!full_sql) {
		return NULL;
	}

	char* p = full_sql;
	int param_idx = 0;
	for (const char* c = sql; *c; c++) {
		if (*c == '?' && params[param_idx]) {
			*p++ = '\'';
			size_t plen = strlen(params[param_idx]);
			memcpy(p, params[param_idx], plen);
			p += plen;
			*p++ = '\'';
			param_idx++;
		} else {
			*p++ = *c;
		}
	}
	*p = '\0';

	uv_mutex_lock(&pool->mutex);
	cJSON* result = csilk_db_query_json_locked(pool, full_sql);
	uv_mutex_unlock(&pool->mutex);

	uint64_t duration = (uv_hrtime() - start) / 1000;
	atomic_fetch_add(&db_duration_us_total, duration);

	if (!result) {
		atomic_fetch_add(&db_errors_total, 1);
	} else {
		atomic_fetch_add(&db_queries_total, 1);
	}

	free(full_sql);
	return result;
}

/* --- Driver Registry --- */

/** @brief Statically-sized registry of registered database drivers (max 16). */
static csilk_db_driver_t* drivers[16];
static int driver_count = 0;
static uv_mutex_t registry_mutex;
static int registry_initialized = 0;

static void
ensure_registry_init(void)
{
	if (!registry_initialized) {
		uv_mutex_init(&registry_mutex);
		registry_initialized = 1;
	}
}

/** @brief Initialise the database subsystem.
 *
 * Registers all built-in drivers (SQLite3, MySQL, PostgreSQL, etc.).
 * Must be called once before any csilk_db_pool_new call.
 * Safe to call multiple times. */
void
csilk_db_init(void)
{
	ensure_registry_init();
	csilk_db_sqlite_init();
#ifdef HAS_MYSQL
	csilk_db_mysql_init();
#endif
#ifdef HAS_POSTGRES
	csilk_db_postgres_init();
#endif
#ifdef HAS_MONGODB
	csilk_db_mongodb_init();
#endif
#ifdef HAS_REDIS
	csilk_db_redis_init();
#endif
}

/** @brief Statically-sized registry of registered database drivers (max 16). */

int
csilk_db_register_driver(const char* name, csilk_db_driver_t* driver)
{
	if (!name || !driver) {
		return -1;
	}
	ensure_registry_init();

	uv_mutex_lock(&registry_mutex);
	if (driver_count >= 16) {
		uv_mutex_unlock(&registry_mutex);
		return -1;
	}
	drivers[driver_count++] = driver;
	uv_mutex_unlock(&registry_mutex);
	return 0;
}

csilk_db_driver_t*
csilk_db_get_driver(const char* name)
{
	if (!name) {
		return NULL;
	}
	ensure_registry_init();

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
