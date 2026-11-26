/**
 * @file test_session_store_lifecycle.c
 * @brief Linux-only regression tests for the session store's
 *        `csilk_once + TLS trampoline` initialization path.
 *
 * Background:
 *   The 2026-11-25 06:59 macOS CI run failed on `test_session_integration`
 *   with "Subprocess aborted" after `init_session_buckets()` was invoked
 *   twice on the same `session_store_t` (once at startup via
 *   csilk_session_init(), once at request time via
 *   csilk_session_start()).  The follow-up commit 2e973ccf ("unify session
 *   bucket init via TLS trampoline + parameterized csilk_once") replaced
 *   `PTHREAD_ONCE_INIT` compound literals with a TLS-trampoline
 *   `csilk_once` pattern so the process-level and per-server stores share
 *   one guarded initialization.
 *
 *   This test lives on Linux only because:
 *     - macOS's libuv backend (uv_once_t) historically exposed the
 *       `PTHREAD_ONCE_INIT` compound-literal issue on macOS-14.
 *     - We want a guard that runs in the *Linux* CI job where the
 *       `csilk_once` → `pthread_once` path is what the production build
 *       actually uses; macOS is a best-effort secondary target.
 *
 * Coverage:
 *   1. Double / N-times `csilk_session_init()` does not abort (regression
 *      for the exact "init_session_buckets called twice" symptom).
 *   2. Same story for `csilk_session_init_server(server)` on the per-server
 *      store slot (built via csilk_app_new + csilk_app_server, which is
 *      the supported public path for creating a server).
 *   3. Concurrent `csilk_session_init()` from N threads — the TLS
 *      trampoline must serialize correctly under pthread_once semantics
 *      and the TLS pending-store write must not race the once guard.
 *   4. Concurrent `csilk_session_init_server(server)` from N threads —
 *      verifies the per-server store slot also survives cross-thread
 *      first-use contention.
 *   5. Interleaved global + per-server inits from the same thread, then
 *      a fresh session on a bare ctx (exercises
 *      `session_store_for_ctx`'s global fallback).
 *
 * @copyright MIT License
 */

#ifdef __linux__

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/app.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/test/test.h"

/* ------------------------------------------------------------------ */
/* 1. N-times csilk_session_init() on the process-level store.         */
/* ------------------------------------------------------------------ */
static void
test_session_init_idempotent(void)
{
    /* The store is lazily allocated on the first call; subsequent calls
     * must not re-run init_session_buckets() — csilk_once guards it. */
    csilk_session_init();
    csilk_session_init();
    csilk_session_init();

    /* Also the explicit re-init path from test_session_cleanup_expired_sweep
     * style: re-init triggers cleanup_expired() but the buckets are not
     * re-locked. */
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != nullptr);
    csilk_session_init();
    csilk_session_start(c);
    assert(csilk_get(c, "_session") != nullptr);
    csilk_session_init(); /* Second call after the request path already ran. */
    csilk_session_init();
    csilk_test_ctx_free(c);

    printf("  PASS: session_init_idempotent\n");
}

/* ------------------------------------------------------------------ */
/* 2. Per-server store: N-times csilk_session_init_server().           */
/* ------------------------------------------------------------------ */
static void
test_session_init_server_idempotent(void)
{
    /* Build a server via the app builder (csilk_server_new() requires a
     * router; csilk_app_new() wires router + server together and is the
     * supported public path for creating a server without running it). */
    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != nullptr);
    csilk_server_t* srv = csilk_app_server(app);
    assert(srv != nullptr);

    csilk_session_init_server(srv);
    csilk_session_init_server(srv);
    csilk_session_init_server(srv);

    /* A bare ctx (no server binding) still routes through the global
     * store; the per-server store is exercised only by the explicit
     * csilk_session_init_server() call above.  Verify the global store
     * is still usable after per-server inits. */
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != nullptr);
    csilk_session_start(c);
    assert(csilk_get(c, "_session") != nullptr);
    csilk_session_init_server(srv); /* Again, after the request path. */
    csilk_session_start(c);
    assert(csilk_get(c, "_session") != nullptr);

    csilk_test_ctx_free(c);
    csilk_app_free(app);

    printf("  PASS: session_init_server_idempotent\n");
}

/* ------------------------------------------------------------------ */
/* 3. Concurrent csilk_session_init() storm.                           */
/* ------------------------------------------------------------------ */
#define NTHREADS 8
#define ITERS 32

typedef struct {
    int tid;
} storm_arg_t;

static void
storm_worker(void* arg)
{
    storm_arg_t* w = (storm_arg_t*)arg;
    for (int i = 0; i < ITERS; i++) {
        csilk_session_init();
        csilk_ctx_t* c = csilk_test_ctx_new();
        assert(c != nullptr);
        csilk_session_start(c);
        assert(csilk_get(c, "_session") != nullptr);
        if ((i % 8) == 0) {
            int val = i * 100 + w->tid;
            csilk_session_set(c, "k", &val);
            int* got = csilk_session_get(c, "k");
            assert(got != nullptr);
            assert(*got == val);
        }
        csilk_test_ctx_free(c);
    }
}

static void
test_session_init_concurrent_storm(void)
{
    csilk_session_init(); /* Pre-warm: forces global store allocation. */

    csilk_thread_t threads[NTHREADS];
    storm_arg_t    args[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        args[i].tid = i;
        assert(csilk_thread_create(&threads[i], storm_worker, &args[i]) == 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        assert(csilk_thread_join(&threads[i]) == 0);
    }

    printf("  PASS: session_init_concurrent_storm\n");
}

/* ------------------------------------------------------------------ */
/* 4. Concurrent per-server init storm.                                */
/* ------------------------------------------------------------------ */
typedef struct {
    csilk_server_t* srv;
    int             tid;
} srv_storm_arg_t;

static void
srv_storm_worker(void* arg)
{
    srv_storm_arg_t* w = (srv_storm_arg_t*)arg;
    for (int i = 0; i < ITERS; i++) {
        csilk_session_init_server(w->srv);
        csilk_session_init(); /* Cross-store contention: both paths. */
    }
}

static void
test_session_init_server_concurrent_storm(void)
{
    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != nullptr);
    csilk_server_t* srv = csilk_app_server(app);
    assert(srv != nullptr);
    csilk_session_init_server(srv); /* Pre-warm. */

    csilk_thread_t  threads[NTHREADS];
    srv_storm_arg_t args[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        args[i].srv = srv;
        args[i].tid = i;
        assert(csilk_thread_create(&threads[i], srv_storm_worker, &args[i]) == 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        assert(csilk_thread_join(&threads[i]) == 0);
    }

    /* After all workers have finished, both stores must still be usable. */
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != nullptr);
    csilk_session_start(c);
    assert(csilk_get(c, "_session") != nullptr);
    csilk_session_init_server(srv);
    csilk_session_start(c);
    assert(csilk_get(c, "_session") != nullptr);
    csilk_test_ctx_free(c);

    csilk_app_free(app);

    printf("  PASS: session_init_server_concurrent_storm\n");
}

/* ------------------------------------------------------------------ */
/* 5. Interleaved global + per-server inits from the same thread.      */
/* ------------------------------------------------------------------ */
static void
test_session_init_interleaved_global_server(void)
{
    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != nullptr);
    csilk_server_t* srv = csilk_app_server(app);
    assert(srv != nullptr);

    /* Interleave: global -> per-server -> global again -> per-server. */
    csilk_session_init();
    csilk_session_init_server(srv);
    csilk_session_init();
    csilk_session_init_server(srv);

    /* Both stores must be usable concurrently from the same thread:
     * a bare ctx routes to the global store. */
    csilk_ctx_t* c_global = csilk_test_ctx_new();
    assert(c_global != nullptr);
    csilk_session_start(c_global);
    assert(csilk_get(c_global, "_session") != nullptr);

    const char* id_glb = csilk_session_get_id(c_global);
    assert(id_glb != nullptr);
    assert(strlen(id_glb) == 36); /* UUID format */

    csilk_test_ctx_free(c_global);
    csilk_app_free(app);

    printf("  PASS: session_init_interleaved_global_server\n");
}

int
main(void)
{
    printf("=== Session Store Lifecycle (Linux-only) ===\n");
    test_session_init_idempotent();
    test_session_init_server_idempotent();
    test_session_init_concurrent_storm();
    test_session_init_server_concurrent_storm();
    test_session_init_interleaved_global_server();
    printf("test_session_store_lifecycle: ALL PASSED\n");
    return 0;
}

#else  /* !__linux__ */

int
main(void)
{
    printf("Skipping test_session_store_lifecycle (not on Linux)\n");
    return 0;
}

#endif /* __linux__ */
