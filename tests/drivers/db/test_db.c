#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "csilk/core/sys_io.h"

void
test_db_init(void)
{
    csilk_db_init();
    printf("test_db_init passed\n");
}

void
test_db_sqlite_create_table(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    int rc = csilk_db_exec(pool,
                           "CREATE TABLE IF NOT EXISTS test_items (id INTEGER "
                           "PRIMARY KEY, value TEXT)");
    assert(rc == 0);

    csilk_db_pool_free(pool);

    /* Clean up test database */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_create_table passed\n");
}

void
test_db_sqlite_insert_query(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    /* Create table */
    csilk_db_exec(pool,
                  "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY "
                  "KEY, name TEXT)");

    /* Insert */
    const char*   params[] = {"Alice", nullptr};
    csilk_json_t* result =
        csilk_db_query_param_json(pool, "INSERT INTO users (name) VALUES (?)", params);
    if (result) {
        csilk_json_free(result);
    }

    /* Query */
    csilk_json_t* users = csilk_db_query_json(pool, "SELECT * FROM users");
    assert(users != nullptr);
    assert(csilk_json_array_size(users) == 1);

    csilk_json_t* user = csilk_json_array_get(users, 0);
    csilk_json_t* name = csilk_json_get(user, "name");
    assert(name != nullptr);
    assert(strcmp(csilk_json_string_value(name), "Alice") == 0);

    csilk_json_free(users);
    csilk_db_pool_free(pool);

    /* Clean up */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_insert_query passed\n");
}

void
test_db_sqlite_transactions(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    csilk_db_exec(pool,
                  "CREATE TABLE IF NOT EXISTS accounts (id INTEGER PRIMARY KEY, "
                  "balance INTEGER)");

    /* Begin transaction */
    csilk_db_exec(pool, "BEGIN");

    /* Insert multiple rows */
    csilk_db_exec(pool, "INSERT INTO accounts (balance) VALUES (100)");
    csilk_db_exec(pool, "INSERT INTO accounts (balance) VALUES (200)");

    /* Commit */
    csilk_db_exec(pool, "COMMIT");

    /* Verify */
    csilk_json_t* accounts =
        csilk_db_query_json(pool, "SELECT SUM(balance) as total FROM accounts");
    assert(accounts != nullptr);
    csilk_json_t* acc = csilk_json_array_get(accounts, 0);
    csilk_json_t* total = csilk_json_get(acc, "total");
    assert(total != nullptr);
    assert(strcmp(csilk_json_string_value(total), "300") == 0);

    csilk_json_free(accounts);
    csilk_db_pool_free(pool);

    /* Clean up */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_transactions passed\n");
}

static int async_called = 0;
static int async_exec_rc = -999;
static int async_exec_called = 0;
static int async_param_query_called = 0;

static void
on_async_query(csilk_json_t* result, void* user_data)
{
    (void)user_data;
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);
    csilk_json_free(result);
    async_called = 1;
}

static void
on_async_exec(int rc, void* user_data)
{
    int* flag = (int*)user_data;
    async_exec_rc = rc;
    if (flag) {
        *flag = 1;
    }
}

static void
on_async_param_query(csilk_json_t* result, void* user_data)
{
    (void)user_data;
    assert(result != nullptr);
    assert(csilk_json_array_size(result) == 1);
    csilk_json_t* row = csilk_json_array_get(result, 0);
    csilk_json_t* msg = csilk_json_get(row, "msg");
    assert(msg != nullptr);
    assert(strcmp(csilk_json_string_value(msg), "param async ok") == 0);
    csilk_json_free(result);
    async_param_query_called = 1;
}

void
test_db_sqlite_async_query(void)
{
    remove("test_async_db.db");
    remove("test_async_db.db-journal");
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_async_db.db");
    assert(pool != nullptr);

    async_exec_called = 0;
    async_exec_rc = -999;
    assert(csilk_db_exec_async(
               pool,
               "CREATE TABLE IF NOT EXISTS async_test (id INTEGER PRIMARY KEY, msg TEXT)",
               on_async_exec,
               &async_exec_called) == 0);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(async_exec_called == 1);
    assert(async_exec_rc == 0);

    /* Stack-local params array goes out of scope immediately before loop runs
     * to verify deep-copy of params inside csilk_db_exec_param_async. */
    async_exec_called = 0;
    async_exec_rc = -999;
    {
        const char* insert_params[] = {"param async ok", nullptr};
        assert(csilk_db_exec_param_async(pool,
                                         "INSERT INTO async_test (msg) VALUES (?)",
                                         insert_params,
                                         on_async_exec,
                                         &async_exec_called) == 0);
    }
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(async_exec_called == 1);
    assert(async_exec_rc == 0);

    async_called = 0;
    int rc = csilk_db_query_json_async(pool, "SELECT * FROM async_test", on_async_query, nullptr);
    assert(rc == 0);

    async_param_query_called = 0;
    {
        const char* query_params[] = {"param async ok", nullptr};
        assert(csilk_db_query_param_json_async(pool,
                                               "SELECT msg FROM async_test WHERE msg = ?",
                                               query_params,
                                               on_async_param_query,
                                               nullptr) == 0);
    }

    /* Run default loop to allow thread pool callbacks to return */
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(async_called == 1);
    assert(async_param_query_called == 1);

    /* Invalid argument checks */
    assert(csilk_db_exec_async(nullptr, "SELECT 1", on_async_exec, nullptr) == -1);
    assert(csilk_db_exec_param_async(pool, nullptr, nullptr, on_async_exec, nullptr) == -1);
    assert(csilk_db_query_param_json_async(pool, "SELECT 1", nullptr, nullptr, nullptr) == -1);

    csilk_db_pool_free(pool);
    remove("test_async_db.db");
    remove("test_async_db.db-journal");
    printf("test_db_sqlite_async_query passed\n");
}

typedef struct {
    int            called;
    csilk_db_err_t err;
    char           msg[64];
    size_t         rows;
} query_ex_ctx_t;

static void
on_query_async_ex(csilk_json_t* result, csilk_db_err_t err, void* user_data)
{
    query_ex_ctx_t* qctx = (query_ex_ctx_t*)user_data;
    qctx->called++;
    qctx->err = err;
    if (result) {
        qctx->rows = csilk_json_array_size(result);
        if (qctx->rows > 0) {
            csilk_json_t* row = csilk_json_array_get(result, 0);
            csilk_json_t* msg = row ? csilk_json_get(row, "msg") : nullptr;
            if (msg && csilk_json_is_string(msg)) {
                snprintf(qctx->msg, sizeof(qctx->msg), "%s", csilk_json_string_value(msg));
            }
        }
        csilk_json_free(result);
    }
}

void
test_db_query_async_api(void)
{
    remove("test_query_async.db");
    remove("test_query_async.db-journal");
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new_ex("sqlite", "test_query_async.db", 2);
    assert(pool != nullptr);
    assert(csilk_db_exec(pool, "CREATE TABLE items (id INTEGER PRIMARY KEY, msg TEXT)") == 0);
    assert(csilk_db_exec(pool, "INSERT INTO items (msg) VALUES ('custom_loop_ok')") == 0);

    /* 1. Raw query on NULL loop (defaults to csilk_io_default_loop) */
    query_ex_ctx_t c_default = {0};
    assert(csilk_db_query_async(pool,
                                "SELECT msg FROM items ORDER BY id",
                                nullptr,
                                nullptr,
                                on_query_async_ex,
                                &c_default) == 0);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(c_default.called == 1);
    assert(c_default.err == CSILK_DB_OK);
    assert(c_default.rows == 1);
    assert(strcmp(c_default.msg, "custom_loop_ok") == 0);

    /* 2. Parameterized + concurrent queries on a custom csilk_io_loop_t */
    csilk_io_loop_t custom_loop;
    assert(csilk_io_loop_init(&custom_loop) == 0);

    query_ex_ctx_t c_param = {0};
    query_ex_ctx_t c_err = {0};
    {
        const char* p[] = {"custom_loop_ok", nullptr};
        assert(csilk_db_query_async(pool,
                                    "SELECT msg FROM items WHERE msg = ?",
                                    p,
                                    &custom_loop,
                                    on_query_async_ex,
                                    &c_param) == 0);
    }
    assert(csilk_db_query_async(pool,
                                "SELECT * FROM nonexistent_tbl_404",
                                nullptr,
                                &custom_loop,
                                on_query_async_ex,
                                &c_err) == 0);
    csilk_io_run(&custom_loop, CSILK_IO_RUN_DEFAULT);
    csilk_io_loop_close(&custom_loop);

    assert(c_param.called == 1);
    assert(c_param.err == CSILK_DB_OK);
    assert(c_param.rows == 1);
    assert(strcmp(c_param.msg, "custom_loop_ok") == 0);

    assert(c_err.called == 1);
    assert(c_err.err != CSILK_DB_OK);
    assert(c_err.rows == 0);

    /* 3. Invalid arguments */
    assert(csilk_db_query_async(
               nullptr, "SELECT 1", nullptr, nullptr, on_query_async_ex, &c_default) == -1);
    assert(csilk_db_query_async(pool, nullptr, nullptr, nullptr, on_query_async_ex, &c_default) ==
           -1);
    assert(csilk_db_query_async(pool, "SELECT 1", nullptr, nullptr, nullptr, &c_default) == -1);

    csilk_db_pool_free(pool);
    remove("test_query_async.db");
    remove("test_query_async.db-journal");
    printf("test_db_query_async_api passed\n");
}

int
main(void)
{
    test_db_init();
    test_db_sqlite_create_table();
    test_db_sqlite_insert_query();
    test_db_sqlite_transactions();
    test_db_sqlite_async_query();
    test_db_query_async_api();
    printf("All db tests passed\n");
    return 0;
}