#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"

/* Pool/client internals (child pid, pipe fds) are needed to assert the
 * liveness of a real subprocess from the regression tests below. */
#include "protocols/mcp/mcp_internal.h"

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

static void
test_pool_argv_differentiation(void)
{
    csilk_mcp_stdio_pool_shutdown();

    char*               argv_a[] = {"npx", "-y", "server-a", NULL};
    char*               argv_b[] = {"npx", "-y", "server-b", NULL};
    csilk_mcp_client_t* ca = csilk_mcp_stdio_pool_acquire("npx", argv_a);
    csilk_mcp_client_t* cb = csilk_mcp_stdio_pool_acquire("npx", argv_b);
    assert(ca != NULL && cb != NULL);
    assert(ca != cb);
    assert(csilk_mcp_stdio_pool_get_count() == 2);

    csilk_mcp_stdio_pool_release(ca);
    csilk_mcp_stdio_pool_release(cb);

    csilk_mcp_client_t* ca2 = csilk_mcp_stdio_pool_acquire("npx", argv_a);
    assert(ca2 == ca);
    csilk_mcp_stdio_pool_release(ca2);
    csilk_mcp_stdio_pool_shutdown();
}

/*
 * NOTE: the helpers below reach into the opaque client only through the public
 * API plus the documented pool internals exposed by mcp_internal.h (pid and
 * pipe fds), so the assertions can inspect a real subprocess's liveness.
 */

static csilk_mcp_client_t*
spawn_real_proc(const char* command, char* const argv[])
{
    csilk_mcp_client_t* c = csilk_mcp_stdio_pool_acquire(command, argv);
    if (!c) {
        return NULL;
    }
    /* Force the lazy stdio spawn: an RPC run forks the child and wires the
     * pipes. A bare `sleep` writes nothing, so the round-trip fails, but the
     * side effect we need (a live child) has happened. */
    char* ignored = csilk_mcp_client_call_tool(c, "noop", "{}");
    free(ignored);
    if (c->child_pid <= 0 || c->pipe_fd[0] < 0 || c->pipe_fd[1] < 0) {
        csilk_mcp_stdio_pool_release(c);
        return NULL;
    }
    return c;
}

/*
 * Regression: an idle pooled client whose subprocess already died must not be
 * handed back out. Otherwise the caller receives a client whose every RPC
 * fails, reported as an opaque tool error instead of a clean reconnect.
 */
static void
test_pool_reaps_dead_child_on_reuse(void)
{
    csilk_mcp_stdio_pool_shutdown();

    /* A long-lived child: the pool treats it as a real MCP stdio server,
     * so the child stays alive until we kill it - exactly the state a
     * crashed MCP server leaves behind in a slot.
     * argv[0] must repeat the command name for execvp to resolve it. */
    char*               proc_argv[] = {"sleep", "100", NULL};
    csilk_mcp_client_t* c1 = spawn_real_proc("sleep", proc_argv);
    if (!c1) {
        /* Environment without a usable sleep(1): skip, do not fail the suite. */
        printf("  (skipped: could not spawn a real subprocess)\n");
        return;
    }
    assert(csilk_mcp_stdio_pool_get_count() == 1);

    /* Kill the child behind its back: the pool slot is now stale but still
     * IDLE, exactly the state a crashed MCP server leaves behind. Do NOT
     * reap it here: a crashed child the parent has not yet waited on is the
     * case the probe must catch, and the pool's own free() path reaps. */
    pid_t victim = c1->child_pid;
    assert(victim > 0);
    kill(victim, SIGKILL);
    /* Let the kernel finish tearing it down so POLLHUP is observable. */
    usleep(50000);

    /* Reacquire must notice the dead child, drop the slot and respawn.
     *
     * A fresh client is spawned lazily on its first RPC, so child_pid is 0 at
     * this point and a fresh malloc may even land on the same address as the
     * old client - pointer identity is NOT a usable invariant here. What must
     * hold is that the pool no longer retains the client whose child we
     * killed, i.e. the old client is gone (its child was reaped by the free
     * path) and the slot was rebuilt from scratch. */
    csilk_mcp_stdio_pool_release(c1);
    csilk_mcp_client_t* c2 = csilk_mcp_stdio_pool_acquire("sleep", proc_argv);
    if (c2) {
        /* The stale slot was dropped and a fresh one reserved: exactly one
         * entry keyed on the same command, and it is IN_USE (not IDLE with a
         * dead child). */
        assert(csilk_mcp_stdio_pool_get_count() == 1);
        assert(csilk_mcp_stdio_pool_get_idle_count() == 0);
        /* Force the fresh spawn (children spawn lazily on first use) and prove
         * the pool built a new process instead of resurrecting the dead one. */
        char* ok = csilk_mcp_client_call_tool(c2, "noop", "{}");
        free(ok); /* sleep writes no JSON-RPC: expect failure, but no hang */
        assert(c2->child_pid > 0);
        assert(c2->child_pid != victim);
        csilk_mcp_stdio_pool_release(c2);
    } else {
        /* Even when respawn is unavailable, the stale slot must be gone. */
        assert(csilk_mcp_stdio_pool_get_count() == 0);
    }
    /* Reap manually if the pool left the victim behind. */
    (void)waitpid(victim, NULL, WNOHANG);
    csilk_mcp_stdio_pool_shutdown();
}

/*
 * Regression: an idle entry older than the TTL is dropped on reuse instead of
 * being resumed, so a subprocess that has been sitting idle unprobed is never
 * handed back out even when nobody calls evict_idle().
 */
static void
test_pool_reaps_ttl_expired_on_reuse(void)
{
    csilk_mcp_stdio_pool_shutdown();

    setenv("CSILK_MCP_STDIO_CACHE_TTL", "1", 1);

    csilk_mcp_client_t* c1 = csilk_mcp_stdio_pool_acquire("ttl_target", NULL);
    assert(c1 != NULL);
    csilk_mcp_stdio_pool_release(c1);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 1);

    /* Within the TTL the same client is reused. */
    csilk_mcp_client_t* same = csilk_mcp_stdio_pool_acquire("ttl_target", NULL);
    assert(same == c1);
    csilk_mcp_stdio_pool_release(same);

    /* Past the TTL the entry is reaped and a fresh client is returned. */
    struct timespec ts = {2, 0};
    nanosleep(&ts, NULL);
    csilk_mcp_client_t* fresh = csilk_mcp_stdio_pool_acquire("ttl_target", NULL);
    assert(fresh != NULL);
    assert(csilk_mcp_stdio_pool_get_count() == 1);
    assert(csilk_mcp_stdio_pool_get_idle_count() == 0);

    csilk_mcp_stdio_pool_release(fresh);
    csilk_mcp_stdio_pool_shutdown();
    unsetenv("CSILK_MCP_STDIO_CACHE_TTL");
}

int
main(void)
{
    test_pool_basic_reuse();
    test_pool_keyed_acquire();
    test_pool_ttl_eviction();
    test_pool_disabled();
    test_pool_argv_differentiation();
    test_pool_reaps_dead_child_on_reuse();
    test_pool_reaps_ttl_expired_on_reuse();
    printf("All test_mcp_stdio_pool tests passed!\n");
    return 0;
}
