#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

void
test_redis_real_connection(void)
{
#ifndef HAS_REDIS
    printf("test_db_redis: SKIPPED (HAS_REDIS not defined)\n");
    return;
#else
    csilk_db_init();

    const char* dsn = getenv("CSILK_TEST_REDIS_DSN");
    if (!dsn || strlen(dsn) == 0) {
        dsn = "host=127.0.0.1;port=6379";
    }

    csilk_db_pool_t* pool = csilk_db_pool_new("redis", dsn);
    if (!pool) {
        if (getenv("CSILK_TEST_REDIS_REQUIRED")) {
            fprintf(stderr, "test_db_redis: FAILED to connect to required Redis (%s)\n", dsn);
            assert(pool != nullptr);
        }
        printf("test_db_redis: SKIPPED (Could not connect to %s)\n", dsn);
        return;
    }

    /* 1. SET command */
    int rc = csilk_db_exec(pool, "SET csilk_test_redis_key hello_csilk_redis");
    assert(rc == 0);

    /* 2. GET command */
    csilk_json_t* rows = csilk_db_query_json(pool, "GET csilk_test_redis_key");
    assert(rows != nullptr);
    assert(csilk_json_is_array(rows));
    assert(csilk_json_array_size(rows) >= 1);
    csilk_json_t* row0 = csilk_json_array_get(rows, 0);
    csilk_json_t* val = csilk_json_get(row0, "value");
    assert(val != nullptr);
    assert(strcmp(csilk_json_string_value(val), "hello_csilk_redis") == 0);
    csilk_json_free(rows);

    /* 3. Storage driver creation & free */
    csilk_storage_driver_t* storage = csilk_redis_storage_driver_new(pool);
    assert(storage != nullptr);
    free(storage);

    /* 4. DEL command */
    rc = csilk_db_exec(pool, "DEL csilk_test_redis_key");
    assert(rc == 0);

    /* 5. Clean up */
    csilk_db_pool_free(pool);
    printf("test_db_redis: PASS\n");
#endif
}

int
main(void)
{
    test_redis_real_connection();
    return 0;
}
