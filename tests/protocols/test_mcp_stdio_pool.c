#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"

static void
test_pool_basic_reuse(void)
{
    csilk_mcp_stdio_pool_shutdown();
    assert(csilk_mcp_stdio_pool_get_count() == 0);

    /* 1. Acquire new client */
    csilk_mcp_client_t* c1 = csilk_mcp_stdio_pool_acquire("mock_srv_1", NULL);
    assert(c1 != NULL);
    assert(csilk_mcp_stdio_pool_get_count() == 1);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 0);

    /* 2. Release back to pool */
    csilk_mcp_stdio_pool_release(c1);
    assert(csilk_mcp_stdio_pool_get_count() == 1);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 1);

    /* 3. Re-acquire with same command should reuse c1 */
    csilk_mcp_client_t* c2 = csilk_mcp_stdio_pool_acquire("mock_srv_1", NULL);
    assert(c2 == c1);
    assert(csilk_mcp_stdio_pool_get_count() == 1);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 0);

    /* 4. Acquire with a different command should spawn c3 */
    csilk_mcp_client_t* c3 = csilk_mcp_stdio_pool_acquire("mock_srv_2", NULL);
    assert(c3 != NULL);
    assert(c3 != c2);
    assert(csilk_mcp_stdio_pool_get_count() == 2);

    csilk_mcp_stdio_pool_release(c2);
    csilk_mcp_stdio_pool_release(c3);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 2);

    /* 5. Clean up */
    int destroyed = csilk_mcp_stdio_pool_shutdown();
    assert(destroyed == 2);
    assert(csilk_mcp_stdio_pool_get_count() == 0);
}

static void
test_pool_keyed_acquire(void)
{
    csilk_mcp_stdio_pool_shutdown();

    csilk_mcp_client_t* u1 = csilk_mcp_stdio_pool_acquire_keyed("u101:s1", "mcp_runner", NULL);
    csilk_mcp_client_t* u2 = csilk_mcp_stdio_pool_acquire_keyed("u102:s1", "mcp_runner", NULL);

    assert(u1 != NULL && u2 != NULL);
    assert(u1 != u2);
    assert(csilk_mcp_stdio_pool_get_count() == 2);

    csilk_mcp_stdio_pool_release(u1);
    csilk_mcp_stdio_pool_release(u2);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 2);

    /* Re-acquire u101:s1 */
    csilk_mcp_client_t* u1_again =
        csilk_mcp_stdio_pool_acquire_keyed("u101:s1", "mcp_runner", NULL);
    assert(u1_again == u1);

    csilk_mcp_stdio_pool_release(u1_again);
    csilk_mcp_stdio_pool_shutdown();
}

static void
test_pool_ttl_eviction(void)
{
    csilk_mcp_stdio_pool_shutdown();

    csilk_mcp_client_t* c = csilk_mcp_stdio_pool_acquire("evict_target", NULL);
    assert(c != NULL);
    csilk_mcp_stdio_pool_release(c);

    /* Inactivity TTL = 0 means immediate eviction of idle */
    int evicted = csilk_mcp_stdio_pool_evict_idle(0);
    assert(evicted == 1);
    assert(csilk_mcp_stdio_pool_get_count() == 0);
}

static void
test_pool_disabled(void)
{
    csilk_mcp_stdio_pool_shutdown();

    setenv("CSILK_MCP_STDIO_MAX_PROCS", "0", 1);

    csilk_mcp_client_t* c = csilk_mcp_stdio_pool_acquire("unpooled_srv", NULL);
    assert(c != NULL);
    /* Should NOT be in the pool */
    assert(csilk_mcp_stdio_pool_get_count() == 0);

    /* Release should directly free c without error */
    csilk_mcp_stdio_pool_release(c);
    assert(csilk_mcp_stdio_pool_get_count() == 0);

    unsetenv("CSILK_MCP_STDIO_MAX_PROCS");
}

int
main(void)
{
    test_pool_basic_reuse();
    test_pool_keyed_acquire();
    test_pool_ttl_eviction();
    test_pool_disabled();
    printf("All test_mcp_stdio_pool tests passed!\n");
    return 0;
}
