#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"
#include "csilk/csilk.h"

void
test_arena_new_free()
{
    printf("Testing csilk_arena_new and csilk_arena_free...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);
    assert(arena != nullptr);
    csilk_arena_free(arena);

    arena = csilk_arena_new(0);
    assert(arena != nullptr);
    csilk_arena_free(arena);

    csilk_arena_free(nullptr);

    printf("csilk_arena_new/free passed!\n");
}

void
test_arena_alloc_basic()
{
    printf("Testing csilk_arena_alloc basic...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);
    assert(arena != nullptr);

    char* s1 = csilk_arena_alloc(arena, 64);
    assert(s1 != nullptr);
    strcpy(s1, "hello arena");

    char* s2 = csilk_arena_alloc(arena, 128);
    assert(s2 != nullptr);
    strcpy(s2, "second allocation");

    assert(strcmp(s1, "hello arena") == 0);
    assert(strcmp(s2, "second allocation") == 0);

    csilk_arena_free(arena);
    printf("csilk_arena_alloc basic passed!\n");
}

void
test_arena_alloc_large()
{
    printf("Testing csilk_arena_alloc large allocation...\n");

    csilk_arena_t* arena = csilk_arena_new(256);
    assert(arena != nullptr);

    char* big = csilk_arena_alloc(arena, 4096);
    assert(big != nullptr);
    memset(big, 'X', 1024);
    big[1023] = '\0';
    assert(strlen(big) == 1023);

    csilk_arena_free(arena);
    printf("csilk_arena_alloc large passed!\n");
}

void
test_arena_alloc_fill()
{
    printf("Testing csilk_arena_alloc fill chunk...\n");

    csilk_arena_t* arena = csilk_arena_new(256);
    assert(arena != nullptr);

    char* a = csilk_arena_alloc(arena, 200);
    assert(a != nullptr);

    char* b = csilk_arena_alloc(arena, 200);
    assert(b != nullptr);

    assert(a != b);

    csilk_arena_free(arena);
    printf("csilk_arena_alloc fill passed!\n");
}

void
test_arena_strdup()
{
    printf("Testing csilk_arena_strdup...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);

    char* s = csilk_arena_strdup(arena, "test string");
    assert(s != nullptr);
    assert(strcmp(s, "test string") == 0);

    assert(csilk_arena_strdup(arena, nullptr) == nullptr);

    csilk_arena_free(arena);
    printf("csilk_arena_strdup passed!\n");
}

void
test_arena_alignment()
{
    printf("Testing csilk_arena_alloc alignment...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);

    for (int i = 0; i < 10; i++) {
        void* ptr = csilk_arena_alloc(arena, 1);
        assert(ptr != nullptr);
        assert(((uintptr_t)ptr & 7) == 0);
    }

    csilk_arena_free(arena);
    printf("csilk_arena_alloc alignment passed!\n");
}

void
test_arena_64_alignment()
{
    printf("Testing csilk_arena_alloc 64-byte alignment...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);
    csilk_arena_set_alignment(arena, 1);

    for (int i = 0; i < 10; i++) {
        void* ptr = csilk_arena_alloc(arena, 1);
        assert(ptr != nullptr);
        assert(((uintptr_t)ptr & 63) == 0);
    }

    /* Toggle back to 8-byte */
    csilk_arena_set_alignment(arena, 0);
    void* ptr8 = csilk_arena_alloc(arena, 1);
    assert(((uintptr_t)ptr8 & 7) == 0);

    /* Toggle from 8-byte to 64-byte */
    csilk_arena_set_alignment(arena, 1);
    void* ptr64 = csilk_arena_alloc(arena, 1);
    assert(((uintptr_t)ptr64 & 63) == 0);

    csilk_arena_free(arena);
    printf("csilk_arena_alloc 64-byte alignment passed!\n");
}

void
test_arena_reset()
{
    printf("Testing csilk_arena_reset...\n");
    csilk_arena_t* arena = csilk_arena_new(1024);
    void*          p1 = csilk_arena_alloc(arena, 100);
    csilk_arena_reset(arena);
    void* p2 = csilk_arena_alloc(arena, 100);
    assert(p1 == p2); // Should reuse the same memory
    csilk_arena_free(arena);
    printf("csilk_arena_reset passed!\n");
}

extern void csilk_arena_flush_free_list(void);

#ifdef TEST_OOM
extern int csilk_arena_get_tls_chunk_count(void);

void
test_arena_tls_cache()
{
    printf("Testing csilk_arena TLS cache...\n");

    csilk_arena_t* arenas[5];
    /* Allocate 5 arenas with standard chunks */
    for (int i = 0; i < 5; i++) {
        arenas[i] = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        csilk_arena_alloc(arenas[i], 100); // Trigger first chunk allocation
    }

    /* Free them all — this should populate the TLS cache */
    for (int i = 0; i < 5; i++) {
        csilk_arena_free(arenas[i]);
    }

    int initial_count = csilk_arena_get_tls_chunk_count();
    assert(initial_count >= 5);

    /* Now allocate new arenas and see if they reuse chunks (count should decrease)
     */
    for (int i = 0; i < 5; i++) {
        csilk_arena_t* arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
        csilk_arena_alloc(arena, 100); // Should reuse from TLS
        assert(csilk_arena_get_tls_chunk_count() < initial_count);
        csilk_arena_free(arena);       // Return back to TLS
        assert(csilk_arena_get_tls_chunk_count() == initial_count);
    }

    printf("csilk_arena TLS cache passed!\n");
}
#endif

void
test_arena_max_total_bytes_tls_enforced(void)
{
    printf("Testing csilk_arena max_total_bytes with TLS cache hit...\n");

    /* Populate TLS cache with a standard chunk */
    csilk_arena_t* a1 = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    assert(a1 != NULL);
    void* p = csilk_arena_alloc(a1, 100);
    assert(p != NULL);
    csilk_arena_free(a1);

    /* Allocate a new arena with a tight size limit smaller than 4KB */
    csilk_arena_t* a2 = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    assert(a2 != NULL);
    assert(csilk_arena_set_max_bytes(a2, 2048) == 0);

    /* Should fail because 4096 > 2048, even though TLS has a cached chunk */
    void* p2 = csilk_arena_alloc(a2, 100);
    assert(p2 == NULL);

    csilk_arena_free(a2);
    printf("csilk_arena max_total_bytes TLS enforcement passed!\n");
}

void
test_arena_default_chunk_size_zero(void)
{
    printf("Testing csilk_arena_new(0) defaults to CSILK_DEFAULT_ARENA_SIZE...\n");

    csilk_arena_t* a = csilk_arena_new(0);
    assert(a != NULL);
    void* p = csilk_arena_alloc(a, 64);
    assert(p != NULL);

    size_t total_size = 0, total_used = 0;
    csilk_arena_get_stats(a, &total_size, &total_used);
    assert(total_size == CSILK_DEFAULT_ARENA_SIZE);

    csilk_arena_free(a);
    printf("csilk_arena_new(0) default chunk size passed!\n");
}

void
test_arena_multi_tier_tls_cache(void)
{
    printf("Testing multi-tier TLS chunk caching (4K, 16K, 64K)...\n");

    /* Flush cache initially */
    csilk_arena_flush_free_list();

    /* Test 16KB tier */
    csilk_arena_t* a16 = csilk_arena_new(16384);
    assert(a16 != NULL);
    csilk_arena_alloc(a16, 5000); /* Will allocate a 16KB chunk */
    csilk_arena_free(a16);        /* Returns 16KB chunk to tier 1 */

    csilk_arena_t* a16_2 = csilk_arena_new(16384);
    void*          p16 = csilk_arena_alloc(a16_2, 5000);
    assert(p16 != NULL);
    csilk_arena_free(a16_2);

    /* Test 64KB tier */
    csilk_arena_t* a64 = csilk_arena_new(65536);
    assert(a64 != NULL);
    csilk_arena_alloc(a64, 30000); /* Will allocate a 64KB chunk */
    csilk_arena_free(a64);         /* Returns 64KB chunk to tier 2 */

    csilk_arena_t* a64_2 = csilk_arena_new(65536);
    void*          p64 = csilk_arena_alloc(a64_2, 30000);
    assert(p64 != NULL);
    csilk_arena_free(a64_2);

    csilk_arena_flush_free_list();
    printf("multi-tier TLS chunk caching passed!\n");
}

#include <pthread.h>

static void*
thread_alloc_and_exit(void* arg)
{
    (void)arg;
    csilk_arena_t* a = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    assert(a != NULL);
    void* p = csilk_arena_alloc(a, 100);
    assert(p != NULL);
    csilk_arena_free(a); /* Pushes chunk to thread TLS */
    return NULL;
}

void
test_arena_pthread_tls_cleanup(void)
{
    printf("Testing pthread key destructor on thread exit...\n");
    pthread_t tid;
    assert(pthread_create(&tid, NULL, thread_alloc_and_exit, NULL) == 0);
    assert(pthread_join(tid, NULL) == 0);
    printf("pthread key destructor passed!\n");
}

void
test_arena_calloc(void)
{
    printf("Testing csilk_arena_calloc...\n");
    csilk_arena_t* a = csilk_arena_new(1024);
    assert(a != NULL);

    int* arr = (int*)csilk_arena_calloc(a, 10, sizeof(int));
    assert(arr != NULL);
    for (int i = 0; i < 10; i++) {
        assert(arr[i] == 0);
    }

    /* Overflow check */
    assert(csilk_arena_calloc(a, SIZE_MAX, 2) == NULL);

    csilk_arena_free(a);
    printf("csilk_arena_calloc passed!\n");
}

int
main()
{
    test_arena_new_free();
    test_arena_alloc_basic();
    test_arena_alloc_large();
    test_arena_alloc_fill();
    test_arena_strdup();
    test_arena_alignment();
    test_arena_64_alignment();
    test_arena_reset();
    test_arena_max_total_bytes_tls_enforced();
    test_arena_default_chunk_size_zero();
    test_arena_multi_tier_tls_cache();
    test_arena_pthread_tls_cleanup();
    test_arena_calloc();
#ifdef TEST_OOM
    test_arena_tls_cache();
#endif
    printf("All arena tests passed!\n");
    return 0;
}
