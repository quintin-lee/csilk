#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "drivers/db/db_internal.h"

#ifdef HAS_POSTGRES
extern csilk_db_driver_t csilk_db_postgres_driver;
#endif

#ifdef HAS_MYSQL
extern csilk_db_driver_t csilk_db_mysql_driver;
#endif

static int  s_mock_set_nb_called = 0;
static int  s_mock_query_async_called = 0;
static int  s_mock_exec_async_called = 0;
static bool s_mock_nb_enabled = false;

static int
mock_connect(csilk_db_pool_t* pool, const char* dsn)
{
    (void)dsn;
    csilk_db_pool_set_connection(pool, (void*)0x1234);
    return 0;
}

static int
mock_disconnect(csilk_db_pool_t* pool)
{
    csilk_db_pool_set_connection(pool, NULL);
    return 0;
}

static int
mock_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
    (void)pool;
    (void)sql;
    (void)result;
    return 0;
}

static int
mock_exec(csilk_db_pool_t* pool, const char* sql)
{
    (void)pool;
    (void)sql;
    return 0;
}

static void
mock_free_result(csilk_db_result_t* result)
{
    (void)result;
}

static int
mock_set_nonblocking(csilk_db_pool_t* pool, int enable)
{
    (void)pool;
    s_mock_set_nb_called++;
    s_mock_nb_enabled = (enable != 0);
    return 0;
}

static int
mock_query_async(csilk_db_pool_t*        pool,
                 const char*             sql,
                 const char**            params,
                 csilk_io_loop_t*        loop,
                 csilk_db_query_async_cb cb,
                 void*                   user_data)
{
    (void)pool;
    (void)sql;
    (void)params;
    (void)loop;
    s_mock_query_async_called++;
    csilk_json_t* arr = csilk_json_array();
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_object(obj, "k", csilk_json_string_new("v"));
    csilk_json_array_append(arr, obj);
    cb(arr, CSILK_DB_OK, user_data);
    return 0;
}

static int
mock_exec_async(csilk_db_pool_t*       pool,
                const char*            sql,
                const char**           params,
                csilk_io_loop_t*       loop,
                csilk_db_exec_async_cb cb,
                void*                  user_data)
{
    (void)pool;
    (void)sql;
    (void)params;
    (void)loop;
    s_mock_exec_async_called++;
    cb(0, user_data);
    return 0;
}

static csilk_db_driver_t s_mock_driver = {
    .name = "mock_nb",
    .connect = mock_connect,
    .disconnect = mock_disconnect,
    .query = mock_query,
    .exec = mock_exec,
    .free_result = mock_free_result,
    .set_nonblocking = mock_set_nonblocking,
    .query_async = mock_query_async,
    .exec_async = mock_exec_async,
};

static void
test_null_and_default_state(void)
{
    assert(csilk_db_is_nonblocking(NULL) == 0);
    assert(csilk_db_set_nonblocking(NULL, 1) == -1);

    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != NULL);
    assert(csilk_db_is_nonblocking(pool) == 0);

    /* SQLite driver does not implement nonblocking socket hooks */
    int rc = csilk_db_set_nonblocking(pool, 1);
    assert(rc == -1);
    assert(csilk_db_is_nonblocking(pool) == 0);

    /* Disabling on an already disabled pool succeeds */
    assert(csilk_db_set_nonblocking(pool, 0) == 0);
    assert(csilk_db_is_nonblocking(pool) == 0);

    csilk_db_pool_free(pool);
    printf("test_null_and_default_state: PASS\n");
}

static void
on_async_query_done(csilk_json_t* res, csilk_db_err_t err, void* user_data)
{
    int* called = (int*)user_data;
    (*called)++;
    assert(err == CSILK_DB_OK);
    assert(res != NULL);
    assert(csilk_json_is_array(res));
    assert(csilk_json_array_size(res) == 1);
    csilk_json_free(res);
}

static void
on_async_query_json_done(csilk_json_t* res, void* user_data)
{
    int* called = (int*)user_data;
    (*called)++;
    assert(res != NULL);
    assert(csilk_json_is_array(res));
    assert(csilk_json_array_size(res) == 1);
    csilk_json_free(res);
}

static void
on_async_exec_done(int rc, void* user_data)
{
    int* called = (int*)user_data;
    (*called)++;
    assert(rc == 0);
}

static void
test_mock_driver_nonblocking(void)
{
    csilk_db_init();
    int reg_rc = csilk_db_register_driver("mock_nb", &s_mock_driver);
    assert(reg_rc == 0);

    csilk_db_pool_t* pool = csilk_db_pool_new("mock_nb", "dummy_dsn");
    assert(pool != NULL);
    assert(csilk_db_is_nonblocking(pool) == 0);

    int rc = csilk_db_set_nonblocking(pool, 1);
    assert(rc == 0);
    assert(s_mock_set_nb_called == 1);
    assert(s_mock_nb_enabled == true);
    assert(csilk_db_is_nonblocking(pool) == 1);

    int query_cb_called = 0;
    rc = csilk_db_query_async(pool, "SELECT 1", NULL, NULL, on_async_query_done, &query_cb_called);
    assert(rc == 0);
    assert(s_mock_query_async_called == 1);
    assert(query_cb_called == 1);

    int query_json_cb_called = 0;
    rc = csilk_db_query_json_async(
        pool, "SELECT 2", on_async_query_json_done, &query_json_cb_called);
    assert(rc == 0);
    assert(s_mock_query_async_called == 2);
    assert(query_json_cb_called == 1);

    int exec_cb_called = 0;
    rc = csilk_db_exec_async(pool, "UPDATE foo SET x=1", on_async_exec_done, &exec_cb_called);
    assert(rc == 0);
    assert(s_mock_exec_async_called == 1);
    assert(exec_cb_called == 1);

    rc = csilk_db_set_nonblocking(pool, 0);
    assert(rc == 0);
    assert(s_mock_set_nb_called == 2);
    assert(s_mock_nb_enabled == false);
    assert(csilk_db_is_nonblocking(pool) == 0);

    csilk_db_pool_free(pool);
    printf("test_mock_driver_nonblocking: PASS\n");
}

static void
test_driver_vtables(void)
{
#ifdef HAS_POSTGRES
    assert(csilk_db_postgres_driver.set_nonblocking != NULL);
    assert(csilk_db_postgres_driver.query_async != NULL);
    assert(csilk_db_postgres_driver.exec_async != NULL);
    printf("test_driver_vtables: PostgreSQL non-blocking hooks verified\n");
#endif

#ifdef HAS_MYSQL
    assert(csilk_db_mysql_driver.set_nonblocking != NULL);
    if (csilk_db_mysql_driver.query_async != NULL) {
        assert(csilk_db_mysql_driver.exec_async != NULL);
    }
    printf("test_driver_vtables: MySQL non-blocking hooks verified\n");
#endif

    printf("test_driver_vtables: PASS\n");
}

int
main(void)
{
    test_null_and_default_state();
    test_mock_driver_nonblocking();
    test_driver_vtables();
    return 0;
}
