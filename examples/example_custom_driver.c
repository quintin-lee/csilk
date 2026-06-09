/**
 * @file example_custom_driver.c
 * @brief Example showing how to implement and register a custom database 
 *        driver using the csilk pluggable database driver interface.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/* --- Custom Driver Implementation --- */

static int
mock_connect(csilk_db_pool_t* pool, const char* dsn)
{
	printf("[MockDB Driver] Connecting to database: %s\n", dsn);
	/* Store dummy connection handle */
	csilk_db_pool_set_connection(pool, (void*)0xDEADBEEF);
	return 0;
}

static int
mock_disconnect(csilk_db_pool_t* pool)
{
	printf("[MockDB Driver] Disconnecting...\n");
	csilk_db_pool_set_connection(pool, NULL);
	return 0;
}

static int
mock_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
	(void)pool;
	printf("[MockDB Driver] Executing query: %s\n", sql);

	result->column_count = 2;
	result->row_count = 2;

	/* Allocate column names */
	result->column_names = malloc(sizeof(char*) * 2);
	result->column_names[0] = strdup("id");
	result->column_names[1] = strdup("name");

	/* Allocate rows */
	result->rows = malloc(sizeof(csilk_db_row_t*) * 2);

	/* Row 1: {id: "1", name: "Alice"} */
	result->rows[0] = malloc(sizeof(csilk_db_row_t));
	result->rows[0]->count = 2;
	result->rows[0]->values = malloc(sizeof(char*) * 2);
	result->rows[0]->values[0] = strdup("1");
	result->rows[0]->values[1] = strdup("Alice");

	/* Row 2: {id: "2", name: "Bob"} */
	result->rows[1] = malloc(sizeof(csilk_db_row_t));
	result->rows[1]->count = 2;
	result->rows[1]->values = malloc(sizeof(char*) * 2);
	result->rows[1]->values[0] = strdup("2");
	result->rows[1]->values[1] = strdup("Bob");

	return 0;
}

static int
mock_exec(csilk_db_pool_t* pool, const char* sql)
{
	(void)pool;
	printf("[MockDB Driver] Executing statement: %s\n", sql);
	return 0;
}

static int
mock_tx_begin(csilk_db_pool_t* pool)
{
	(void)pool;
	printf("[MockDB Driver] Transaction Begin\n");
	return 0;
}

static int
mock_tx_commit(csilk_db_pool_t* pool)
{
	(void)pool;
	printf("[MockDB Driver] Transaction Commit\n");
	return 0;
}

static int
mock_tx_rollback(csilk_db_pool_t* pool)
{
	(void)pool;
	printf("[MockDB Driver] Transaction Rollback\n");
	return 0;
}

static void
mock_free_result(csilk_db_result_t* result)
{
	if (!result) {
		return;
	}
	if (result->column_names) {
		for (int i = 0; i < result->column_count; i++) {
			free(result->column_names[i]);
		}
		free(result->column_names);
	}
	if (result->rows) {
		for (int i = 0; i < result->row_count; i++) {
			if (result->rows[i]) {
				if (result->rows[i]->values) {
					for (int j = 0; j < result->rows[i]->count; j++) {
						free(result->rows[i]->values[j]);
					}
					free(result->rows[i]->values);
				}
				free(result->rows[i]);
			}
		}
		free(result->rows);
	}
}

/* Define driver operations struct */
static csilk_db_driver_t mock_driver = {
    .name = "mockdb",
    .connect = mock_connect,
    .disconnect = mock_disconnect,
    .query = mock_query,
    .exec = mock_exec,
    .transaction_begin = mock_tx_begin,
    .transaction_commit = mock_tx_commit,
    .transaction_rollback = mock_tx_rollback,
    .free_result = mock_free_result,
};

/* --- Application Code --- */

int
main(void)
{
	/* Initialize database system */
	csilk_db_init();

	/* Register our custom database driver */
	if (csilk_db_register_driver("mockdb", &mock_driver) != 0) {
		fprintf(stderr, "Failed to register mockdb driver\n");
		return 1;
	}
	printf("[System] Registered 'mockdb' driver successfully.\n");

	/* Connect to database using our mock driver */
	csilk_db_pool_t* pool = csilk_db_pool_new("mockdb", "mock://localhost/testdb");
	if (!pool) {
		fprintf(stderr, "Failed to connect to mockdb pool\n");
		return 1;
	}

	/* Execute a query and fetch results as JSON */
	cJSON* users = csilk_db_query_json(pool, "SELECT * FROM users");
	if (users) {
		char* json_str = cJSON_Print(users);
		printf("[Result] Query output (JSON):\n%s\n", json_str);
		free(json_str);
		cJSON_Delete(users);
	} else {
		fprintf(stderr, "Failed to execute query\n");
	}

	/* Execute DDL / non-query statement */
	csilk_db_exec(pool, "UPDATE users SET active = 1");

	/* Close connection and cleanup */
	csilk_db_pool_free(pool);
	printf("[System] Connection pool closed.\n");

	return 0;
}
