#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/ctx/context.h"
#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "modules/db/db_internal.h"

#ifdef HAS_POSTGRES
extern csilk_db_driver_t csilk_db_postgres_driver;
#endif

#ifdef HAS_MYSQL
extern csilk_db_driver_t csilk_db_mysql_driver;
#endif

static int              s_mock_set_nb_called = 0;
static int              s_mock_query_async_called = 0;
static int              s_mock_exec_async_called = 0;
static bool             s_mock_nb_enabled = false;
static csilk_io_loop_t* s_mock_query_async_loop_received = NULL;
static csilk_io_loop_t* s_mock_exec_async_loop_received = NULL;

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
    s_mock_query_async_loop_received = loop;
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
    s_mock_exec_async_loop_received = loop;
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

static void
test_concurrent_async_leases_same_thread(void)
{
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("mock_nb", "dummy_dsn");
    assert(pool != NULL);

    /* Populate multi-connections in mock pool */
    pool->max_conns = 3;
    pool->connections[0] = (void*)0x1001;
    pool->connections[1] = (void*)0x1002;
    pool->connections[2] = (void*)0x1003;

    void* c1 = NULL;
    int   l1 = 0;
    int   rc1 = csilk_db_pool_acquire_async(pool, &c1, &l1);
    assert(rc1 == 0 && c1 == (void*)0x1001 && l1 > 0);

    void* c2 = NULL;
    int   l2 = 0;
    int   rc2 = csilk_db_pool_acquire_async(pool, &c2, &l2);
    assert(rc2 == 0 && c2 == (void*)0x1002 && l2 > 0);
    assert(c1 != c2);
    assert(l1 != l2);

    void* c3 = NULL;
    int   l3 = 0;
    int   rc3 = csilk_db_pool_acquire_async(pool, &c3, &l3);
    assert(rc3 == 0 && c3 == (void*)0x1003 && l3 > 0);
    assert(c2 != c3);

    /* Pool full: 4th acquire should fail */
    void* c4 = NULL;
    int   l4 = 0;
    int   rc4 = csilk_db_pool_acquire_async(pool, &c4, &l4);
    assert(rc4 == -1 && c4 == NULL);

    /* Return c2 and re-acquire */
    csilk_db_pool_put_async(pool, c2, l2, true);

    void* c5 = NULL;
    int   l5 = 0;
    int   rc5 = csilk_db_pool_acquire_async(pool, &c5, &l5);
    assert(rc5 == 0 && c5 == (void*)0x1002 && l5 > 0);

    /* Clean up */
    csilk_db_pool_put_async(pool, c1, l1, true);
    csilk_db_pool_put_async(pool, c3, l3, true);
    csilk_db_pool_put_async(pool, c5, l5, true);

    csilk_db_pool_free(pool);
    printf("test_concurrent_async_leases_same_thread: PASS\n");
}

static void* s_resumed_conn = NULL;
static int   s_resumed_lease = 0;

static void
on_async_resumed(void* arg, void* raw_conn, int lease_id)
{
    int* flag = (int*)arg;
    *flag = 1;
    s_resumed_conn = raw_conn;
    s_resumed_lease = lease_id;
}

static void
test_async_pool_fifo_queue(void)
{
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("mock_nb", "dummy_dsn");
    assert(pool != NULL);

    pool->max_conns = 1;
    pool->connections[0] = (void*)0x2001;

    void* c1 = NULL;
    int   l1 = 0;
    assert(csilk_db_pool_acquire_async(pool, &c1, &l1) == 0);
    assert(c1 == (void*)0x2001);

    /* Queue a waiter when pool is exhausted */
    int resume_called = 0;
    s_resumed_conn = NULL;
    s_resumed_lease = 0;
    assert(csilk_db_pool_queue_waiter(pool, on_async_resumed, &resume_called) == 0);

    /* Release c1; waiter should be immediately popped and dispatched */
    csilk_db_pool_put_async(pool, c1, l1, true);

    assert(resume_called == 1);
    assert(s_resumed_conn == (void*)0x2001);
    assert(s_resumed_lease > 0);
    assert(s_resumed_lease != l1);

    /* Release the resumed lease */
    csilk_db_pool_put_async(pool, s_resumed_conn, s_resumed_lease, true);

    csilk_db_pool_free(pool);
    printf("test_async_pool_fifo_queue: PASS\n");
}

static void
test_poisoned_connection_eviction(void)
{
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("mock_nb", "dummy_dsn");
    assert(pool != NULL);

    pool->max_conns = 1;
    pool->connections[0] = (void*)0x3001;

    void* c1 = NULL;
    int   l1 = 0;
    assert(csilk_db_pool_acquire_async(pool, &c1, &l1) == 0);

    /* Return as unhealthy */
    csilk_db_pool_put_async(pool, c1, l1, false);

    /* Slot should be evicted (connections[0] == NULL) */
    assert(pool->connections[0] == NULL);

    csilk_db_pool_free(pool);
    printf("test_poisoned_connection_eviction: PASS\n");
}

static void
test_ctx_get_loop_and_auto_resolution(void)
{
    /* 1. NULL context returns default loop */
    csilk_io_loop_t* l1 = csilk_ctx_get_loop(NULL);
    assert(l1 != NULL);
    assert(l1 == csilk_io_default_loop());

    /* 2. Mock query_async automatically receives non-null loop even when caller passes NULL */
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("mock_nb", "dummy_dsn");
    assert(pool != NULL);
    assert(csilk_db_set_nonblocking(pool, 1) == 0);

    s_mock_query_async_loop_received = NULL;
    int cb_called = 0;
    int rc = csilk_db_query_async(pool, "SELECT 1", NULL, NULL, on_async_query_done, &cb_called);
    assert(rc == 0);
    assert(s_mock_query_async_loop_received != NULL);
    assert(s_mock_query_async_loop_received == csilk_io_default_loop());

    s_mock_exec_async_loop_received = NULL;
    int exec_called = 0;
    rc = csilk_db_exec_param_async(pool, "UPDATE foo", NULL, on_async_exec_done, &exec_called);
    assert(rc == 0);
    assert(s_mock_exec_async_loop_received != NULL);
    assert(s_mock_exec_async_loop_received == csilk_io_default_loop());

    csilk_db_pool_free(pool);
    printf("test_ctx_get_loop_and_auto_resolution: PASS\n");
}

int
main(void)
{
    test_null_and_default_state();
    test_mock_driver_nonblocking();
    test_driver_vtables();
    test_concurrent_async_leases_same_thread();
    test_async_pool_fifo_queue();
    test_poisoned_connection_eviction();
    test_ctx_get_loop_and_auto_resolution();
    return 0;
}
