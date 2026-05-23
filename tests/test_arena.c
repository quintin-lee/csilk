#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

void test_arena_new_free() {
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

void test_arena_alloc_basic() {
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

void test_arena_alloc_large() {
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

void test_arena_alloc_fill() {
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

void test_arena_strdup() {
  printf("Testing csilk_arena_strdup...\n");

  csilk_arena_t* arena = csilk_arena_new(1024);

  char* s = csilk_arena_strdup(arena, "test string");
  assert(s != NULL);
  assert(strcmp(s, "test string") == 0);

  assert(csilk_arena_strdup(arena, NULL) == NULL);

  csilk_arena_free(arena);
  printf("csilk_arena_strdup passed!\n");
}

void test_arena_alignment() {
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

void test_arena_reset() {
  printf("Testing csilk_arena_reset...\n");
  csilk_arena_t* arena = csilk_arena_new(1024);
  void* p1 = csilk_arena_alloc(arena, 100);
  csilk_arena_reset(arena);
  void* p2 = csilk_arena_alloc(arena, 100);
  assert(p1 == p2); // Should reuse the same memory
  csilk_arena_free(arena);
  printf("csilk_arena_reset passed!\n");
}

int main() {
  test_arena_new_free();
  test_arena_alloc_basic();
  test_arena_alloc_large();
  test_arena_alloc_fill();
  test_arena_strdup();
  test_arena_alignment();
  test_arena_reset();
  printf("All arena tests passed!\n");
  return 0;
}
