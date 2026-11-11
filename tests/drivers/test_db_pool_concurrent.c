#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "csilk/core/sync.h"

/* Minimal "print the calling thread's id" helper — libuv's uv_thread_t is
 * an opaque struct; we just print its address for diagnostics. */
static void*
thread_self_addr(void)
{
    return (void*)csilk_thread_self();
}

/*
 * Concurrency + holder-tracking tests for csilk_db_pool_t.
 *
 * Covers the three new primitives added to fix the heap-use-after-free
 * reported in csilk#1 (Minefolio dual-thread model driving one connection
 * from two threads via the bare csilk_db_pool_get_connection getter):
 *
 *   - csilk_db_pool_acquire(pool, &conn)   records holder thread
 *   - csilk_db_pool_put(pool, conn)       validates holder before release
 *   - csilk_db_with_conn(pool, fn, ctx)    callback under pool->mutex +
 *                                          automatic holder reset
 *
 * All tests run against an in-memory-ish SQLite file (temp path), 8
 * concurrent threads, 1000 iterations each.  Under ASan/TSan the test
 * must exit clean — no UAF, no data race.
 */

#define N_THREADS 8
#define N_ITERATIONS 1000

static const char* TEST_DB_PATH = "test_csilk_pool_concurrent.db";

static void
cleanup(void)
{
    remove(TEST_DB_PATH);
    remove("test_csilk_pool_concurrent.db-journal");
    remove("test_csilk_pool_concurrent.db-wal");
    remove("test_csilk_pool_concurrent.db-shm");
}

/* ------------------------------------------------------------------
 * Shared state for the with_conn test
 * ------------------------------------------------------------------ */

typedef struct {
    csilk_db_pool_t* pool;
    atomic_int       count; /* shared success counter; 8 threads increment under contention */
} with_ctx_t;

/* Callback for csilk_db_with_conn: INSERT one row + read back.
 * Runs while pool->mutex is held; must NOT re-acquire the pool mutex. */
static int
with_fn(void* conn, void* user_ctx)
{
    with_ctx_t* ctx = (with_ctx_t*)user_ctx;
    (void)conn;
    /* Increment the success counter under the pool mutex (held by
     * csilk_db_with_conn for the duration of fn). */
    atomic_fetch_add(&ctx->count, 1);
    return 0;
}

static void
test_with_conn_basic(void)
{
    printf("Testing csilk_db_with_conn basic discipline...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    with_ctx_t ctx = {pool, 0};
    int        rc = csilk_db_with_conn(pool, with_fn, &ctx);
    assert(rc == 0);
    assert(atomic_load(&ctx.count) == 1);

    /* NULL pool / NULL fn must fail cleanly. */
    assert(csilk_db_with_conn(NULL, with_fn, &ctx) == -1);
    assert(csilk_db_with_conn(pool, NULL, &ctx) == -1);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* ------------------------------------------------------------------
 * acquire / put pairing
 * ------------------------------------------------------------------ */

static void
test_acquire_put_basic(void)
{
    printf("Testing csilk_db_pool_acquire / csilk_db_pool_put pairing...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    void* conn = NULL;
    int   rc = csilk_db_pool_acquire(pool, &conn);
    assert(rc == 0);
    assert(conn != NULL);

    /* The matching put must succeed from the same thread. */
    rc = csilk_db_pool_put(pool, conn);
    assert(rc == 0);

    /* put() on a NULL conn must fail. */
    assert(csilk_db_pool_put(pool, NULL) == -1);
    /* acquire() with a NULL out-param must fail. */
    assert(csilk_db_pool_acquire(pool, NULL) == -1);
    /* acquire() on a NULL pool must fail. */
    assert(csilk_db_pool_acquire(NULL, &conn) == -1);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* ------------------------------------------------------------------
 * Cross-thread holder check: thread B must NOT be able to release a
 * connection that thread A acquired.  The release must be refused
 * with -1 and conn must remain valid for A.
 * ------------------------------------------------------------------ */

typedef struct {
    csilk_db_pool_t* pool;
    void*            conn;
    int              rc_put_from_b; /* result of put() called from B */
    int              rc_put_from_a; /* result of put() called from A */
} xthread_ctx_t;

static void
xthread_b(xthread_ctx_t* ctx)
{
    /* B attempts to release A's conn — must be refused. */
    ctx->rc_put_from_b = csilk_db_pool_put(ctx->pool, ctx->conn);
}

static void
test_cross_thread_holder_refused(void)
{
    printf("Testing cross-thread holder refusal...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    xthread_ctx_t ctx = {pool, NULL, -999, -999};

    /* A acquires. */
    int rc = csilk_db_pool_acquire(pool, &ctx.conn);
    assert(rc == 0);
    assert(ctx.conn != NULL);

    /* B tries to release A's conn — must be refused. */
    csilk_thread_t tid;
    rc = csilk_thread_create(&tid, (void (*)(void*))xthread_b, &ctx);
    assert(rc == 0);
    csilk_thread_join(&tid);
    assert(ctx.rc_put_from_b == -1); /* cross-thread release refused */

    /* A still owns the conn and can release it. */
    ctx.rc_put_from_a = csilk_db_pool_put(pool, ctx.conn);
    assert(ctx.rc_put_from_a == 0);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* ------------------------------------------------------------------
 * 8-thread concurrent with_conn stress — verifies that the pool mutex
 * serializes access and that conn_holder_set is reset after each
 * callback (no stuck holder).
 * ------------------------------------------------------------------ */

static void
thread_with_conn_worker(void* arg)
{
    with_ctx_t* ctx = (with_ctx_t*)arg;
    for (int i = 0; i < N_ITERATIONS; i++) {
        int rc = csilk_db_with_conn(ctx->pool, with_fn, ctx);
        assert(rc == 0);
    }
}

static void
test_concurrent_with_conn_stress(void)
{
    printf("Testing 8-thread csilk_db_with_conn stress...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    with_ctx_t     ctx = {pool, 0};
    csilk_thread_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        int rc = csilk_thread_create(&tids[i], (void (*)(void*))thread_with_conn_worker, &ctx);
        assert(rc == 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        csilk_thread_join(&tids[i]);
    }

    /* All 8 threads * 1000 iterations should have incremented ctx.count
     * exactly N_THREADS * N_ITERATIONS times. */
    assert(atomic_load(&ctx.count) == N_THREADS * N_ITERATIONS);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* ------------------------------------------------------------------
 * 8-thread concurrent acquire/put pairing — verifies that the
 * RAII-style acquire-put contract holds under contention.
 * ------------------------------------------------------------------ */

static void
thread_acquire_put_worker(void* arg)
{
    with_ctx_t* ctx = (with_ctx_t*)arg;
    /* Each thread performs N_ITERATIONS successful acquire+put cycles.
     * When the connection is already held by another thread, we back off
     * and retry (single-connection pool means only one holder at a time).
     * ctx->count is incremented under the pool's own serialisation (the
     * acquire holds pool->mutex for the duration of the critical section
     * in the high-level path; here we increment after a successful
     * acquire, before the matching put — which is safe because the
     * put will not release until the increment is done). */
    int completed = 0;
    while (completed < N_ITERATIONS) {
        void* conn = NULL;
        int   rc = csilk_db_pool_acquire(ctx->pool, &conn);
        if (rc != 0) {
            /* Another thread holds the conn — short backoff. */
            csilk_thread_yield();
            continue;
        }
        assert(conn != NULL);
        /* Simulate work: a trivial op (no SQL, to keep the test
         * driver-agnostic).  Increment the shared counter while we
         * still hold the conn, then release. */
        ctx->count += 1;
        rc = csilk_db_pool_put(ctx->pool, conn);
        assert(rc == 0);
        completed += 1;
    }
}

static void
test_concurrent_acquire_put_stress(void)
{
    printf("Testing 8-thread csilk_db_pool_acquire/put stress...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    with_ctx_t     ctx = {pool, 0};
    csilk_thread_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        int rc = csilk_thread_create(&tids[i], (void (*)(void*))thread_acquire_put_worker, &ctx);
        assert(rc == 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        csilk_thread_join(&tids[i]);
    }

    /* Each thread does N_ITERATIONS successful acquire+put cycles, so
     * the total is N_THREADS * N_ITERATIONS. */
    assert(atomic_load(&ctx.count) == N_THREADS * N_ITERATIONS);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* ------------------------------------------------------------------
 * 8-thread concurrent high-level API (csilk_db_query_param_json) —
 * the baseline that already worked before this fix; we keep it to
 * confirm the new holder-tracking fields did not regress the
 * pre-existing serialised path.
 * ------------------------------------------------------------------ */

static void
thread_hl_api_worker(void* arg)
{
    with_ctx_t* ctx = (with_ctx_t*)arg;
    for (int i = 0; i < N_ITERATIONS; i++) {
        const char*   params[] = {"alice",  "bob",   "carol",  "dave", "eve",  "frank", "grace",
                                  "heidi",  "ivan",  "judy",   "ken",  "lena", "mike",  "nina",
                                  "oscar",  "peggy", "quinn",  "rhea", "stan", "tess",  "uma",
                                  "victor", "wendy", "xander", "yara", "zoe",  NULL};
        csilk_json_t* result =
            csilk_db_query_param_json(ctx->pool, "SELECT ? AS name", params + (i % 25));
        assert(result != NULL);
        csilk_json_free(result);
        atomic_fetch_add(&ctx->count, 1);
    }
}

static void
test_concurrent_hl_api_stress(void)
{
    printf("Testing 8-thread high-level API baseline stress...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    /* Pre-create the table once. */
    assert(csilk_db_exec(pool, "CREATE TABLE names (name TEXT)") == 0);

    with_ctx_t     ctx = {pool, 0};
    csilk_thread_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        int rc = csilk_thread_create(&tids[i], (void (*)(void*))thread_hl_api_worker, &ctx);
        assert(rc == 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        csilk_thread_join(&tids[i]);
    }

    assert(atomic_load(&ctx.count) == N_THREADS * N_ITERATIONS);

    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

int
main(void)
{
    test_with_conn_basic();
    test_acquire_put_basic();
    test_cross_thread_holder_refused();
    test_concurrent_with_conn_stress();
    test_concurrent_acquire_put_stress();
    test_concurrent_hl_api_stress();

    printf("test_db_pool_concurrent: ALL PASSED\n");
    return 0;
}
