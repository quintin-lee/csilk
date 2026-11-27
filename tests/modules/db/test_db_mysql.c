#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "modules/db/db_internal.h"

static void
test_mysql_interpolate_params_unit(void)
{
#ifndef HAS_MYSQL
    printf("test_mysql_interpolate_params_unit: SKIPPED (HAS_MYSQL not defined)\n");
    return;
#else
    const char* p1[] = {"42", "Alice", nullptr};
    char*       res = csilk_mysql_interpolate_params(
        nullptr, "SELECT * FROM users WHERE id = ? AND name = ?", p1);
    assert(res != nullptr);
    assert(strcmp(res, "SELECT * FROM users WHERE id = '42' AND name = 'Alice'") == 0);
    free(res);

    /* Test quote escaping */
    const char* p2[] = {"O'Reilly", nullptr};
    res = csilk_mysql_interpolate_params(nullptr, "SELECT * FROM users WHERE data = ?", p2);
    assert(res != nullptr);
    assert(strcmp(res, "SELECT * FROM users WHERE data = 'O\\'Reilly'") == 0);
    free(res);

    /* Test literal quote skipping */
    const char* p3[] = {"10", nullptr};
    res = csilk_mysql_interpolate_params(
        nullptr, "SELECT 'what?is?this' FROM users WHERE id = ?", p3);
    assert(res != nullptr);
    assert(strcmp(res, "SELECT 'what?is?this' FROM users WHERE id = '10'") == 0);
    free(res);

    /* Test null param */
    const char* p4[] = {"1", nullptr, nullptr};
    res = csilk_mysql_interpolate_params(
        nullptr, "SELECT * FROM users WHERE id = ? AND desc = ?", p4);
    assert(res != nullptr);
    assert(strcmp(res, "SELECT * FROM users WHERE id = '1' AND desc = NULL") == 0);
    free(res);

    printf("test_mysql_interpolate_params_unit: PASS\n");
#endif
}

#ifdef HAS_MYSQL
typedef struct {
    int  count;
    bool done;
} mysql_async_test_state_t;

static void
on_mysql_async_exec_cb(int res_rc, void* udata)
{
    int* ed = (int*)udata;
    assert(res_rc == 0);
    *ed = 1;
}

static void
on_mysql_async_query_cb(csilk_json_t* res, csilk_db_err_t err, void* udata)
{
    mysql_async_test_state_t* st = (mysql_async_test_state_t*)udata;
    assert(err == CSILK_DB_OK);
    assert(res != nullptr);
    assert(csilk_json_array_size(res) >= 1);
    csilk_json_free(res);
    st->count++;
    if (st->count == 10) {
        st->done = true;
    }
}
#endif

void
test_mysql_real_connection(void)
{
#ifndef HAS_MYSQL
    printf("test_db_mysql: SKIPPED (HAS_MYSQL not defined)\n");
    return;
#else
    csilk_db_init();

    const char* dsn = getenv("CSILK_TEST_MYSQL_DSN");
    if (!dsn || strlen(dsn) == 0) {
        dsn = "host=127.0.0.1;port=3306;user=csilk_user;password=csilk_password;dbname=csilk_test";
    }

    csilk_db_pool_t* pool = csilk_db_pool_new("mysql", dsn);
    if (!pool) {
        if (getenv("CSILK_TEST_MYSQL_REQUIRED")) {
            fprintf(stderr, "test_db_mysql: FAILED to connect to required MySQL (%s)\n", dsn);
            assert(pool != nullptr);
        }
        printf("test_db_mysql: SKIPPED (Could not connect to %s)\n", dsn);
        return;
    }

    /* 1. DDL: Create table */
    csilk_db_exec(pool, "DROP TABLE IF EXISTS test_mysql_items");
    int rc = csilk_db_exec(pool,
                           "CREATE TABLE test_mysql_items ("
                           "  id INT AUTO_INCREMENT PRIMARY KEY,"
                           "  name VARCHAR(64) NOT NULL,"
                           "  score INT NOT NULL"
                           ")");
    assert(rc == 0);

    /* 2. DML: Insert row */
    rc = csilk_db_exec(pool, "INSERT INTO test_mysql_items (name, score) VALUES ('Alice', 100)");
    assert(rc == 0);

    /* 3. Parameterized insert */
    const char* insert_params[] = {"Bob", "95", nullptr};
    rc = csilk_db_exec_param(
        pool, "INSERT INTO test_mysql_items (name, score) VALUES (?, ?)", insert_params);
    assert(rc == 0);

    /* 4. Query and verify JSON results */
    csilk_json_t* rows =
        csilk_db_query_json(pool, "SELECT name, score FROM test_mysql_items ORDER BY score DESC");
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
    csilk_json_t* q_res = csilk_db_query_param_json(
        pool, "SELECT score FROM test_mysql_items WHERE name = ?", q_params);
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
    rc = csilk_db_exec(pool, "INSERT INTO test_mysql_items (name, score) VALUES ('Ghost', 50)");
    assert(rc == 0);
    rc = csilk_db_exec(pool, "ROLLBACK");
    assert(rc == 0);

    const char*   ghost_params[] = {"Ghost", nullptr};
    csilk_json_t* ghost_res = csilk_db_query_param_json(
        pool, "SELECT name FROM test_mysql_items WHERE name = ?", ghost_params);
    assert(ghost_res != nullptr);
    assert(csilk_json_array_size(ghost_res) == 0);
    csilk_json_free(ghost_res);

    /* 7. Non-blocking mode check */
    rc = csilk_db_set_nonblocking(pool, 1);
    if (rc == 0) {
        assert(csilk_db_is_nonblocking(pool) == 1);
        rc = csilk_db_set_nonblocking(pool, 0);
        assert(rc == 0);
        assert(csilk_db_is_nonblocking(pool) == 0);
    } else {
        /* Standard Oracle MySQL client without non-blocking extension */
        assert(csilk_db_is_nonblocking(pool) == 0);
    }

    /* 8. Async query & exec tests */
    csilk_io_loop_t* loop = csilk_io_default_loop();
    int              exec_done = 0;
    rc = csilk_db_exec_async(pool,
                             "INSERT INTO test_mysql_items (name, score) VALUES ('AsyncUser', 88)",
                             on_mysql_async_exec_cb,
                             &exec_done);
    assert(rc == 0);
    while (!exec_done) {
        csilk_io_run(loop, CSILK_IO_RUN_ONCE);
    }
    assert(exec_done == 1);

    /* Parameterized async query */
    mysql_async_test_state_t astate = {.count = 0, .done = false};
    const char*              async_q_params[] = {"AsyncUser", nullptr};
    for (int i = 0; i < 10; i++) {
        rc = csilk_db_query_async(pool,
                                  "SELECT score FROM test_mysql_items WHERE name = ?",
                                  async_q_params,
                                  loop,
                                  on_mysql_async_query_cb,
                                  &astate);
        assert(rc == 0);
    }
    while (!astate.done) {
        csilk_io_run(loop, CSILK_IO_RUN_ONCE);
    }
    assert(astate.count == 10);

    /* 9. Clean up */
    csilk_db_exec(pool, "DROP TABLE test_mysql_items");
    csilk_db_pool_free(pool);
    printf("test_db_mysql: PASS\n");
#endif
}

int
main(void)
{
    test_mysql_interpolate_params_unit();
    test_mysql_real_connection();
    return 0;
}
