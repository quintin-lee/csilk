#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

static int mock_connect_called = 0;
static int mock_disconnect_called = 0;

static int
mock_connect(csilk_db_pool_t* pool, const char* dsn)
{
    (void)dsn;
    mock_connect_called++;
    csilk_db_pool_set_connection(pool, (void*)0xDEAD);
    return 0;
}

static int
mock_disconnect(csilk_db_pool_t* pool)
{
    mock_disconnect_called++;
    csilk_db_pool_set_connection(pool, nullptr);
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

static int
mock_tx_begin(csilk_db_pool_t* pool)
{
    (void)pool;
    return 0;
}
static int
mock_tx_commit(csilk_db_pool_t* pool)
{
    (void)pool;
    return 0;
}
static int
mock_tx_rollback(csilk_db_pool_t* pool)
{
    (void)pool;
    return 0;
}

static void
mock_free_result(csilk_db_result_t* result)
{
    (void)result;
}

static csilk_db_driver_t mock_db_driver = {
    .name = "mock_db",
    .connect = mock_connect,
    .disconnect = mock_disconnect,
    .query = mock_query,
    .exec = mock_exec,
    .transaction_begin = mock_tx_begin,
    .transaction_commit = mock_tx_commit,
    .transaction_rollback = mock_tx_rollback,
    .free_result = mock_free_result,
};

static void
test_db_init_registers_sqlite(void)
{
    printf("Testing csilk_db_init registers sqlite...\n");

    csilk_db_init();

    csilk_db_driver_t* d = csilk_db_get_driver("sqlite");
    assert(d != nullptr);
    assert(strcmp(d->name, "sqlite") == 0);

    printf("  passed\n");
}

static void
test_db_get_driver_nonexistent(void)
{
    printf("Testing csilk_db_get_driver nonexistent...\n");

    csilk_db_driver_t* d = csilk_db_get_driver("does_not_exist");
    assert(d == nullptr);

    printf("  passed\n");
}

static void
test_db_register_custom_driver(void)
{
    printf("Testing csilk_db_register_driver custom...\n");

    int r = csilk_db_register_driver("mock_db", &mock_db_driver);
    assert(r == 0);

    csilk_db_driver_t* d = csilk_db_get_driver("mock_db");
    assert(d != nullptr);
    assert(d == &mock_db_driver);

    printf("  passed\n");
}

static void
test_db_register_duplicate(void)
{
    printf("Testing csilk_db_register_driver duplicate is idempotent...\n");

    int r = csilk_db_register_driver("mock_db", &mock_db_driver);
    assert(r == 0 || r == -1);

    csilk_db_driver_t* d = csilk_db_get_driver("mock_db");
    assert(d != nullptr);

    printf("  passed\n");
}

static void
test_db_pool_new_with_mock(void)
{
    printf("Testing csilk_db_pool_new with mock driver...\n");

    mock_connect_called = 0;
    mock_disconnect_called = 0;

    csilk_db_pool_t* pool = csilk_db_pool_new("mock_db", "mock://localhost");
    assert(pool != nullptr);
    assert(mock_connect_called == 1);

    void* conn = csilk_db_pool_get_connection(pool);
    assert(conn == (void*)0xDEAD);

    csilk_db_pool_free(pool);
    assert(mock_disconnect_called == 1);

    printf("  passed\n");
}

static void
test_db_pool_new_unknown_driver(void)
{
    printf("Testing csilk_db_pool_new with unknown driver...\n");

    csilk_db_pool_t* pool = csilk_db_pool_new("unknown_db", "some://dsn");
    assert(pool == nullptr);

    printf("  passed\n");
}

static void
test_db_stats(void)
{
    printf("Testing csilk_db_get_stats...\n");

    csilk_db_stats_t stats;
    memset(&stats, 0xFF, sizeof(stats));
    csilk_db_get_stats(&stats);

    printf("  queries=%lu execs=%lu errors=%lu\n",
           (unsigned long)stats.queries_total,
           (unsigned long)stats.execs_total,
           (unsigned long)stats.errors_total);

    printf("  passed\n");
}

int
main(void)
{
    test_db_init_registers_sqlite();
    test_db_get_driver_nonexistent();
    test_db_register_custom_driver();
    test_db_register_duplicate();
    test_db_pool_new_with_mock();
    test_db_pool_new_unknown_driver();
    test_db_stats();

    printf("test_db_registry: ALL PASSED\n");
    return 0;
}
