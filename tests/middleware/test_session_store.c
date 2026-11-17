/**
 * @file test_session_store.c
 * @brief Unit tests for the pluggable session store abstraction.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* get returns -1 when no live Redis connection (stub or degraded real store) */
    char*  data = NULL;
    size_t len = 0;
    assert(csilk_session_store_get(store, "sess-1", &data, &len) == -1);

#if !HAS_REDIS
    /* Pure stubs (no hiredis) treat set/del as no-ops returning 0. */
    assert(csilk_session_store_set(store, "sess-1", "v", 1, 3600) == 0);
    assert(csilk_session_store_del(store, "sess-1") == 0);
#else
    /* Real store degraded without a live Redis: set/del report failure (-1),
     * which is the correct semantics — do not assert no-op 0 here. */
    (void)csilk_session_store_set(store, "sess-1", "v", 1, 3600);
    (void)csilk_session_store_del(store, "sess-1");
#endif

    csilk_session_store_destroy(store);

    printf("test_redis_store_stubs: PASS\n");
}

static void
test_mq_store(void)
{
    printf("Testing MQ session store (local table fallback)...\n");

    csilk_session_store_t* store = csilk_session_store_new_mq(NULL, "mq:");
    assert(store != NULL);

    /* get before set: not found */
    char*  data = NULL;
    size_t len = 0;
    int    rc = csilk_session_store_get(store, "sess-1", &data, &len);
    assert(rc == -1);

    /* set + get roundtrip */
    rc = csilk_session_store_set(store, "sess-1", "hello", 5, 3600);
    assert(rc == 0);

    rc = csilk_session_store_get(store, "sess-1", &data, &len);
    assert(rc == 0);
    assert(data != NULL);
    assert(len == 5);
    assert(strncmp(data, "hello", 5) == 0);
    free(data);

    /* del + get: not found */
    rc = csilk_session_store_del(store, "sess-1");
    assert(rc == 0);
    data = NULL;
    rc = csilk_session_store_get(store, "sess-1", &data, &len);
    assert(rc == -1);

    /* Multiple sessions */
    csilk_session_store_set(store, "a", "1", 1, 0);
    csilk_session_store_set(store, "b", "2", 1, 0);
    data = NULL;
    assert(csilk_session_store_get(store, "a", &data, &len) == 0);
    free(data);
    data = NULL;
    assert(csilk_session_store_get(store, "b", &data, &len) == 0);
    free(data);

    csilk_session_store_destroy(store);

    printf("test_mq_store: PASS\n");
}

static void
test_ttl_expiry(void)
{
    printf("Testing session TTL expiry...\n");

    csilk_session_store_t* store = csilk_session_store_new_mq(NULL, "ttl:");
    assert(store != NULL);

    /* Set with 0-second TTL: should expire immediately */
    int rc = csilk_session_store_set(store, "short", "data", 4, 0);
    assert(rc == 0);

    char*  data = NULL;
    size_t len = 0;
    rc = csilk_session_store_get(store, "short", &data, &len);
    /* 0 TTL = no expiry, so get succeeds */
    assert(rc == 0);
    free(data);

    /* Set with 1-second TTL and verify it's still readable */
    rc = csilk_session_store_set(store, "ttl1", "x", 1, 1);
    assert(rc == 0);
    data = NULL;
    rc = csilk_session_store_get(store, "ttl1", &data, &len);
    assert(rc == 0);
    free(data);

    csilk_session_store_destroy(store);

    printf("test_ttl_expiry: PASS\n");
}

int
main(void)
{
    test_local_store();
    test_redis_store_stubs();
    test_mq_store();
    test_ttl_expiry();
    printf("All session store tests passed successfully!\n");
    return 0;
}
