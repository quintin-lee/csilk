#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "drivers/db/db_internal.h"

static void
test_sql_rewrite_placeholders_unit(void)
{
    int   count = 0;
    char* res =
        csilk_sql_rewrite_placeholders("SELECT * FROM users WHERE id = ? AND name = ?", &count);
    assert(res != nullptr);
    assert(count == 2);
    assert(strcmp(res, "SELECT * FROM users WHERE id = $1 AND name = $2") == 0);
    free(res);

    /* Test literal quote skipping */
    count = 0;
    res = csilk_sql_rewrite_placeholders("SELECT 'what?is?this' FROM users WHERE id = ?", &count);
    assert(res != nullptr);
    assert(count == 1);
    assert(strcmp(res, "SELECT 'what?is?this' FROM users WHERE id = $1") == 0);
    free(res);

    /* Test escaped quotes in string literal */
    count = 0;
    res = csilk_sql_rewrite_placeholders("SELECT 'O''Reilly?test' WHERE x = ? AND y = ?", &count);
    assert(res != nullptr);
    assert(count == 2);
    assert(strcmp(res, "SELECT 'O''Reilly?test' WHERE x = $1 AND y = $2") == 0);
    free(res);

    printf("test_sql_rewrite_placeholders_unit: PASS\n");
}

void
test_postgres_real_connection(void)
{
#ifndef HAS_POSTGRES
    printf("test_db_postgres: SKIPPED (HAS_POSTGRES not defined)\n");
    return;
#else
    csilk_db_init();

    const char* dsn = getenv("CSILK_TEST_POSTGRES_DSN");
    if (!dsn || strlen(dsn) == 0) {
        dsn = "host=127.0.0.1 port=5432 user=csilk_user password=csilk_password dbname=csilk_test";
    }

    csilk_db_pool_t* pool = csilk_db_pool_new("postgres", dsn);
    if (!pool) {
        if (getenv("CSILK_TEST_POSTGRES_REQUIRED")) {
            fprintf(
                stderr, "test_db_postgres: FAILED to connect to required PostgreSQL (%s)\n", dsn);
            assert(pool != nullptr);
        }
        printf("test_db_postgres: SKIPPED (Could not connect to %s)\n", dsn);
        return;
    }

    /* 1. DDL: Create table */
    csilk_db_exec(pool, "DROP TABLE IF EXISTS test_pg_items");
    int rc = csilk_db_exec(pool,
                           "CREATE TABLE test_pg_items ("
                           "  id SERIAL PRIMARY KEY,"
                           "  name VARCHAR(64) NOT NULL,"
                           "  score INT NOT NULL"
                           ")");
    assert(rc == 0);

    /* 2. DML: Insert row */
    rc = csilk_db_exec(pool, "INSERT INTO test_pg_items (name, score) VALUES ('Alice', 100)");
    assert(rc == 0);

    /* 3. Parameterized insert (? is rewritten to $1, $2 by postgres driver) */
    const char* insert_params[] = {"Bob", "95", nullptr};
    rc = csilk_db_exec_param(
        pool, "INSERT INTO test_pg_items (name, score) VALUES (?, ?)", insert_params);
    assert(rc == 0);

    /* 4. Query and verify JSON results */
    csilk_json_t* rows =
        csilk_db_query_json(pool, "SELECT name, score FROM test_pg_items ORDER BY score DESC");
    assert(rows != nullptr);
    assert(csilk_json_is_array(rows));
    assert(csilk_json_array_size(rows) == 2);

    csilk_json_t* row0 = csilk_json_array_get(rows, 0);
    assert(strcmp(csilk_json_string_value(csilk_json_get(row0, "name")), "Alice") == 0);
    assert(strcmp(csilk_json_string_value(csilk_json_get(row0, "score")), "100") == 0);

    csilk_json_t* row1 = csilk_json_array_get(rows, 1);
    assert(strcmp(csilk_json_string_value(csilk_json_get(row1, "name")), "Bob") == 0);
    assert(strcmp(csilk_json_string_value(csilk_json_get(row1, "score")), "95") == 0);
    csilk_json_free(rows);

    /* 5. Parameterized query */
    const char*   q_params[] = {"Alice", nullptr};
    csilk_json_t* q_res =
        csilk_db_query_param_json(pool, "SELECT score FROM test_pg_items WHERE name = ?", q_params);
    assert(q_res != nullptr);
    assert(csilk_json_array_size(q_res) == 1);
    csilk_json_t* q_row = csilk_json_array_get(q_res, 0);
    csilk_json_t* score_val = csilk_json_get(q_row, "score");
    assert(score_val != nullptr);
    assert(csilk_json_as_int64(score_val, 0) == 100);
    csilk_json_free(q_res);

    /* 6. Transaction: Rollback */
    rc = csilk_db_exec(pool, "BEGIN");
    assert(rc == 0);
    rc = csilk_db_exec(pool, "INSERT INTO test_pg_items (name, score) VALUES ('Ghost', 50)");
    assert(rc == 0);
    rc = csilk_db_exec(pool, "ROLLBACK");
    assert(rc == 0);

    const char*   ghost_params[] = {"Ghost", nullptr};
    csilk_json_t* ghost_res = csilk_db_query_param_json(
        pool, "SELECT name FROM test_pg_items WHERE name = ?", ghost_params);
    assert(ghost_res != nullptr);
    assert(csilk_json_array_size(ghost_res) == 0);
    csilk_json_free(ghost_res);

    /* 7. Clean up */
    csilk_db_exec(pool, "DROP TABLE test_pg_items");
    csilk_db_pool_free(pool);
    printf("test_db_postgres: PASS\n");
#endif
}

int
main(void)
{
    test_sql_rewrite_placeholders_unit();
    test_postgres_real_connection();
    return 0;
}
