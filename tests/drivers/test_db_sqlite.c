#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

static const char* TEST_DB_PATH = "test_csilk_sqlite.db";

static void
cleanup(void)
{
    remove(TEST_DB_PATH);
    remove("test_csilk_sqlite.db-journal");
}

static void
test_sqlite_pool_lifecycle(void)
{
    printf("Testing SQLite pool lifecycle...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_exec_and_query(void)
{
    printf("Testing SQLite exec + query_json...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    int r =
        csilk_db_exec(pool, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)");
    assert(r == 0);

    r = csilk_db_exec(pool, "INSERT INTO users (name, age) VALUES ('Alice', 30)");
    assert(r == 0);

    r = csilk_db_exec(pool, "INSERT INTO users (name, age) VALUES ('Bob', 25)");
    assert(r == 0);

    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM users ORDER BY id");
    assert(result != nullptr);
    assert(csilk_json_is_array(result));
    assert(csilk_json_array_size(result) == 2);

    csilk_json_t* first = csilk_json_array_get(result, 0);
    assert(first != nullptr);
    csilk_json_t* name = csilk_json_get(first, "name");
    assert(name != nullptr);
    assert(strcmp(csilk_json_string_value(name), "Alice") == 0);

    csilk_json_free(result);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_param_query(void)
{
    printf("Testing SQLite parameterized query...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    csilk_db_exec(pool, "CREATE TABLE items (id INTEGER PRIMARY KEY, label TEXT)");
    csilk_db_exec(pool, "INSERT INTO items (label) VALUES ('alpha')");
    csilk_db_exec(pool, "INSERT INTO items (label) VALUES ('beta')");
    csilk_db_exec(pool, "INSERT INTO items (label) VALUES ('gamma')");

    const char*   params[] = {"beta", nullptr};
    csilk_json_t* result =
        csilk_db_query_param_json(pool, "SELECT * FROM items WHERE label = ?", params);
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);

    csilk_json_t* row = csilk_json_array_get(result, 0);
    assert(strcmp(csilk_json_get_string(row, "label"), "beta") == 0);

    csilk_json_free(result);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_transaction_commit(void)
{
    printf("Testing SQLite transaction commit...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    csilk_db_exec(pool, "CREATE TABLE tx_test (val TEXT)");

    csilk_db_exec(pool, "BEGIN");
    csilk_db_exec(pool, "INSERT INTO tx_test (val) VALUES ('tx_data')");
    csilk_db_exec(pool, "COMMIT");

    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM tx_test");
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);

    csilk_json_t* row = csilk_json_array_get(result, 0);
    assert(strcmp(csilk_json_get_string(row, "val"), "tx_data") == 0);

    csilk_json_free(result);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_transaction_rollback(void)
{
    printf("Testing SQLite transaction rollback...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    csilk_db_exec(pool, "CREATE TABLE rb_test (val TEXT)");
    csilk_db_exec(pool, "INSERT INTO rb_test (val) VALUES ('committed')");

    csilk_db_exec(pool, "BEGIN");
    csilk_db_exec(pool, "INSERT INTO rb_test (val) VALUES ('rolled_back')");
    csilk_db_exec(pool, "ROLLBACK");

    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM rb_test");
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);

    csilk_json_t* row = csilk_json_array_get(result, 0);
    assert(strcmp(csilk_json_get_string(row, "val"), "committed") == 0);

    csilk_json_free(result);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_invalid_sql(void)
{
    printf("Testing SQLite invalid SQL...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    int r = csilk_db_exec(pool, "INVALID SQL STATEMENT");
    assert(r == -1);

    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM nonexistent_table");
    assert(result == nullptr);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

static void
test_sqlite_multiple_columns(void)
{
    printf("Testing SQLite multi-column types...\n");

    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    csilk_db_exec(
        pool,
        "CREATE TABLE types_test (id INTEGER PRIMARY KEY, name TEXT, score REAL, active INTEGER)");
    csilk_db_exec(pool, "INSERT INTO types_test (name, score, active) VALUES ('test', 99.5, 1)");

    csilk_json_t* result = csilk_db_query_json(pool, "SELECT * FROM types_test");
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);

    csilk_json_t* row = csilk_json_array_get(result, 0);
    assert(csilk_json_get(row, "name") != nullptr);
    assert(csilk_json_get(row, "score") != nullptr);
    assert(csilk_json_get(row, "active") != nullptr);

    csilk_json_free(result);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

int
main(void)
{
    test_sqlite_pool_lifecycle();
    test_sqlite_exec_and_query();
    test_sqlite_param_query();
    test_sqlite_transaction_commit();
    test_sqlite_transaction_rollback();
    test_sqlite_invalid_sql();
    test_sqlite_multiple_columns();

    printf("test_db_sqlite: ALL PASSED\n");
    return 0;
}
