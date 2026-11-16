/**
 * @file test_session_store.c
 * @brief Unit tests for the pluggable session store abstraction.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/csilk.h"
#include "csilk/core/session_store.h"
#include "csilk/test/test.h"

static void
test_local_store(void)
{
    printf("Testing local session store...\n");

    csilk_session_store_t* store = csilk_session_store_new_local();
    assert(store != NULL);

    /* get returns -1 (not found) */
    char*  data = NULL;
    size_t len = 0;
    int    rc = csilk_session_store_get(store, "sess-123", &data, &len);
    assert(rc == -1);

    /* set succeeds (no-op for local) */
    rc = csilk_session_store_set(store, "sess-123", "value", 5, 3600);
    assert(rc == 0);

    /* del succeeds */
    rc = csilk_session_store_del(store, "sess-123");
    assert(rc == 0);

    /* Null args */
    assert(csilk_session_store_get(NULL, "id", &data, &len) == -1);
    assert(csilk_session_store_set(NULL, "id", "v", 1, 0) == -1);
    assert(csilk_session_store_del(NULL, "id") == -1);

    csilk_session_store_destroy(store);
    csilk_session_store_destroy(NULL); /* null-safe */

    printf("test_local_store: PASS\n");
}

static void
test_redis_store_stubs(void)
{
    printf("Testing Redis session store stubs...\n");

    csilk_session_store_t* store = csilk_session_store_new_redis("127.0.0.1", 6379, "csilk:");
    assert(store != NULL);

    /* get returns -1 (not implemented) */
    char*  data = NULL;
    size_t len = 0;
    assert(csilk_session_store_get(store, "sess-1", &data, &len) == -1);

    /* set/del are no-ops returning 0 */
    assert(csilk_session_store_set(store, "sess-1", "v", 1, 3600) == 0);
    assert(csilk_session_store_del(store, "sess-1") == 0);

    csilk_session_store_destroy(store);

    printf("test_redis_store_stubs: PASS\n");
}

static void
test_mq_store_stubs(void)
{
    printf("Testing MQ session store stubs...\n");

    csilk_session_store_t* store = csilk_session_store_new_mq(NULL, "mq:");
    assert(store != NULL);

    char*  data = NULL;
    size_t len = 0;
    assert(csilk_session_store_get(store, "sess-1", &data, &len) == -1);
    assert(csilk_session_store_set(store, "sess-1", "v", 1, 3600) == 0);
    assert(csilk_session_store_del(store, "sess-1") == 0);

    csilk_session_store_destroy(store);

    printf("test_mq_store_stubs: PASS\n");
}

int
main(void)
{
    test_local_store();
    test_redis_store_stubs();
    test_mq_store_stubs();
    printf("All session store tests passed successfully!\n");
    return 0;
}
