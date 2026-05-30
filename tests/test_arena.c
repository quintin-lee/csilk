#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/core/srv_types.h"
#include "csilk/csilk.h"

void
test_arena_new_free()
{
	printf("Testing csilk_arena_new and csilk_arena_free...\n");

	csilk_arena_t* arena = csilk_arena_new(1024);
	assert(arena != NULL);
	csilk_arena_free(arena);

	arena = csilk_arena_new(0);
	assert(arena != NULL);
	csilk_arena_free(arena);

	csilk_arena_free(NULL);

	printf("csilk_arena_new/free passed!\n");
}

void
test_arena_alloc_basic()
{
	printf("Testing csilk_arena_alloc basic...\n");

	csilk_arena_t* arena = csilk_arena_new(1024);
	assert(arena != NULL);

	char* s1 = csilk_arena_alloc(arena, 64);
	assert(s1 != NULL);
	strcpy(s1, "hello arena");

	char* s2 = csilk_arena_alloc(arena, 128);
	assert(s2 != NULL);
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
	assert(arena != NULL);

	char* big = csilk_arena_alloc(arena, 4096);
	assert(big != NULL);
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
	assert(arena != NULL);

	char* a = csilk_arena_alloc(arena, 200);
	assert(a != NULL);

	char* b = csilk_arena_alloc(arena, 200);
	assert(b != NULL);

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
	assert(s != NULL);
	assert(strcmp(s, "test string") == 0);

	assert(csilk_arena_strdup(arena, NULL) == NULL);

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
		assert(ptr != NULL);
		assert(((uintptr_t)ptr & 7) == 0);
	}

	csilk_arena_free(arena);
	printf("csilk_arena_alloc alignment passed!\n");
}

void
test_arena_reset()
{
	printf("Testing csilk_arena_reset...\n");
	csilk_arena_t* arena = csilk_arena_new(1024);
	void* p1 = csilk_arena_alloc(arena, 100);
	csilk_arena_reset(arena);
	void* p2 = csilk_arena_alloc(arena, 100);
	assert(p1 == p2); // Should reuse the same memory
	csilk_arena_free(arena);
	printf("csilk_arena_reset passed!\n");
}

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
		csilk_arena_free(arena); // Return back to TLS
		assert(csilk_arena_get_tls_chunk_count() == initial_count);
	}

	printf("csilk_arena TLS cache passed!\n");
}
#endif

int
main()
{
	test_arena_new_free();
	test_arena_alloc_basic();
	test_arena_alloc_large();
	test_arena_alloc_fill();
	test_arena_strdup();
	test_arena_alignment();
	test_arena_reset();
#ifdef TEST_OOM
	test_arena_tls_cache();
#endif
	printf("All arena tests passed!\n");
	return 0;
}
