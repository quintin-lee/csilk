/**
 * @file example_db.c
 * @brief Database example using SQLite3 built-in driver.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/** @brief Drop and create users table. */
static int
setup_db(csilk_db_pool_t* pool)
{
    // Drop existing table if any
    csilk_db_exec(pool, "DROP TABLE IF EXISTS users");

    // Create table
    if (csilk_db_exec(pool,
                      "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, "
                      "email TEXT)") != 0) {
        fprintf(stderr, "Failed to create table\n");
        return -1;
    }

    // Insert sample data
    const char* insert_sql = "INSERT INTO users (name, email) VALUES (?, ?)";
    const char* params[] = {"Alice", "alice@example.com", nullptr};

    csilk_json_t* result = csilk_db_query_param_json(pool, insert_sql, params);
    if (result) {
        csilk_json_free(result);
    }

    params[0] = "Bob";
    params[1] = "bob@example.com";
    result = csilk_db_query_param_json(pool, insert_sql, params);
    if (result) {
        csilk_json_free(result);
    }

    return 0;
}

/** @brief Query and display all users. */
static void
query_users(csilk_db_pool_t* pool)
{
    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM users");
    if (!result) {
        fprintf(stderr, "Query failed\n");
        return;
    }

    printf("Users:\n");
    csilk_json_t* item = nullptr;
    for (size_t _i = 0; _i < csilk_json_array_size(result); _i++) {
        csilk_json_t* item = csilk_json_array_get(result, _i);
        if (!item) {
            break;
        }
        const char* name = csilk_json_get_string(item, "name");
        const char* email = csilk_json_get_string(item, "email");
        printf("  %s - %s\n", name, email);
    }

    csilk_json_free(result);
}

/** @brief Main entry point. */
int
main(void)
{
    // Initialize database system (registers SQLite3 driver)
    csilk_db_init();

    // Create database pool
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test.db");
    if (!pool) {
        fprintf(stderr, "Failed to create database pool\n");
        return 1;
    }

    printf("Database connected to test.db\n");

    // Setup table
    if (setup_db(pool) != 0) {
        csilk_db_pool_free(pool);
        return 1;
    }

    // Query users
    query_users(pool);

    // Cleanup
    csilk_db_pool_free(pool);
    printf("Database closed\n");

    return 0;
}
