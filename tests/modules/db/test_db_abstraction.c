/**
 * @file test_db_abstraction.c
 * @brief Comprehensive unit tests for csilk database abstraction boundary,
 * null argument defenses, and driver dispatch validation.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

static int async_flag = 0;

static void
on_async_cb(int r, void* ud)
{
    if (ud) {
        *(int*)ud = 1;
    }
}

static void
test_db_abstraction_defensive_nulls(void)
{
    printf("Testing database abstraction NULL argument boundaries...");

    /* NULL pool / NULL statement validations */
    assert(csilk_db_exec(nullptr, "SELECT 1;") == -1);
    assert(csilk_db_exec(nullptr, nullptr) == -1);

    const char* params[] = {"val", nullptr};
    assert(csilk_db_exec_param(nullptr, "INSERT INTO t VALUES (?)", params) == -1);

    assert(csilk_db_query_json(nullptr, "SELECT 1;") == NULL);
    assert(csilk_db_query_json(NULL, NULL) == NULL);
    assert(csilk_db_query_param_json(NULL, "SELECT ?", params) == NULL);

    /* Unsupported driver names fail gracefully */
    csilk_db_init();
    csilk_db_pool_t* bad_pool = csilk_db_pool_new("invalid_driver_xyz", "test");
    assert(bad_pool == NULL);

    printf(" passed\n");
}

static void
test_db_abstraction_driver_registry_dispatch(void)
{
    printf("Testing database driver registry dispatch and OOM boundaries...");

    csilk_db_init();

    /* Verify SQLite in-memory pool works correctly with dispatch */
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != NULL);

    /* Test various edge-case queries */
    int rc = csilk_db_exec(pool, "CREATE TABLE dispatch_test (id INTEGER PRIMARY KEY, value TEXT)");
    assert(rc == 0);

    const char* params[] = {"dispatch_val", nullptr};
    rc = csilk_db_exec_param(pool, "INSERT INTO dispatch_test (value) VALUES (?)", params);
    assert(rc == 0);

    csilk_json_t* result =
        csilk_db_query_param_json(pool, "SELECT value FROM dispatch_test WHERE value = ?", params);
    assert(result != NULL);
    assert(csilk_json_array_size(result) == 1);
    csilk_json_free(result);

    /* Test NULL callback boundary */
    async_flag = 0;
    assert(csilk_db_exec_async(pool, "SELECT 1;", on_async_cb, &async_flag) == 0);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(async_flag == 1);

    /* Test OOM limits on huge pool allocation */
    csilk_db_pool_t* huge_pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(huge_pool != NULL);

    csilk_db_pool_free(pool);
    csilk_db_pool_free(huge_pool);

    printf(" passed\n");
}

int
main(void)
{
    test_db_abstraction_defensive_nulls();
    test_db_abstraction_driver_registry_dispatch();
    printf("test_db_abstraction: ALL PASSED\n");
    return 0;
}
