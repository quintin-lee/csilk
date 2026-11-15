/**
 * @file test_mvcc_cache_snapshot.c
 * @brief Multi-writer concurrent MVCC cache snapshot consistency & garbage collection
 * boundary interception tests.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/cache/mvcc_cache.h"
#include "csilk/test/test.h"

#undef assert
#define assert(expr)                                                                               \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(                                                                               \
                stderr, "Assertion failed: (%s), file %s, line %d\n", #expr, __FILE__, __LINE__);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#define SNAP_WRITERS 4
#define SNAP_OPS 5000
#define SNAP_KEYS 32

typedef struct {
    uint32_t key;
    uint32_t val;
} snap_t;

typedef struct {
    csilk_mvcc_cache_t*  cache;
    atomic_uint_fast64_t writes;
    atomic_uint_fast64_t reads;
    atomic_uint_fast64_t corrupted;
    atomic_bool          stop_readers;
} snap_ctx_t;

static void*
snap_writer(void* arg)
{
    snap_ctx_t* ctx = (snap_ctx_t*)arg;
    /* Derive a per-writer base from the argument pointer (portable across
     * Linux and macOS where pthread_t has different underlying types). */
    uint32_t base = (uint32_t)((uintptr_t)ctx % 7) + 1;

    for (int i = 0; i < SNAP_OPS; i++) {
        uint32_t k = (base + i) % SNAP_KEYS;
        snap_t   s = {.key = k, .val = (base << 16) | i};

        char buf[32];
        snprintf(buf, sizeof(buf), "snap:%u", k);
        if (csilk_mvcc_cache_set(ctx->cache, buf, &s, sizeof(s)) == 0) {
            atomic_fetch_add_explicit(&ctx->writes, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void*
snap_reader(void* arg)
{
    snap_ctx_t* ctx = (snap_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->stop_readers, memory_order_relaxed)) {
        uint32_t k = (rand() % SNAP_KEYS);
        char     buf[32];
        snprintf(buf, sizeof(buf), "snap:%u", k);

        csilk_mvcc_view_t view;
        if (csilk_mvcc_cache_get_view(ctx->cache, buf, &view) == 0) {
            /* Verify snapshot integrity under high-concurrency writes */
            const snap_t* s = (const snap_t*)view.data;
            if (s->key != k) {
                atomic_fetch_add_explicit(&ctx->corrupted, 1, memory_order_relaxed);
            }
            csilk_mvcc_cache_release_view(ctx->cache, &view);
            atomic_fetch_add_explicit(&ctx->reads, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

int
main(void)
{
    printf("Testing MVCC Cache Multi-Writer concurrent snapshot integrity & garbage collection...");

    csilk_mvcc_cache_t* cache = csilk_mvcc_cache_new(16);
    assert(cache != NULL);

    /* Pre-populate keys */
    for (int i = 0; i < SNAP_KEYS; i++) {
        char   buf[32];
        snap_t s = {.key = (uint32_t)i, .val = 0};
        snprintf(buf, sizeof(buf), "snap:%u", (uint32_t)i);
        assert(csilk_mvcc_cache_set(cache, buf, &s, sizeof(s)) == 0);
    }

    snap_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cache = cache;
    atomic_init(&ctx.writes, 0);
    atomic_init(&ctx.reads, 0);
    atomic_init(&ctx.corrupted, 0);
    atomic_init(&ctx.stop_readers, false);

    pthread_t writers[SNAP_WRITERS];
    pthread_t reader;

    pthread_create(&reader, NULL, snap_reader, &ctx);
    for (int i = 0; i < SNAP_WRITERS; i++) {
        pthread_create(&writers[i], NULL, snap_writer, &ctx);
    }

    for (int i = 0; i < SNAP_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    atomic_store_explicit(&ctx.stop_readers, true, memory_order_relaxed);
    pthread_join(reader, NULL);

    /* Verify no corrupted snapshots were seen under heavy multi-writer races */
    assert(atomic_load(&ctx.corrupted) == 0);
    assert(atomic_load(&ctx.writes) == (uint64_t)SNAP_WRITERS * SNAP_OPS);
    assert(atomic_load(&ctx.reads) > 0);

    /* Verify garbage collection reclaims all retired versions cleanly */
    csilk_mvcc_cache_free(cache);

    /* Verify cache boundary with NULL argument intercepts */
    assert(csilk_mvcc_cache_get_view(NULL, "key", NULL) == -1);
    assert(csilk_mvcc_cache_set(NULL, "key", "val", 4) == -1);
    assert(csilk_mvcc_cache_delete(NULL, "key") == -1);
    /* new(0) must still return a valid object even with a zero bucket count;
     * keep the handle so we can release it and avoid a leak under LSAN. */
    csilk_mvcc_cache_t* edge = csilk_mvcc_cache_new(0);
    assert(edge != NULL);
    csilk_mvcc_cache_free(edge);

    printf(" passed\n");
    printf("test_mvcc_cache_snapshot: ALL PASSED\n");
    return 0;
}
