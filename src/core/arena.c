/**
 * @file arena.c
 * @brief Arena allocator implementation for request-scoped memory.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk_internal.h"

/** @brief A single chunk in the arena linked list. */
typedef struct csilk_arena_chunk_s {
  struct csilk_arena_chunk_s* next; /**< Pointer to next chunk. */
  size_t size;                      /**< Total size of this chunk. */
  size_t used;                      /**< Bytes used in this chunk. */
  uint8_t data[];                   /**< Flexible array for chunk data. */
} csilk_arena_chunk_t;

/** @brief Arena allocator for request-scoped memory. */
typedef struct csilk_arena_s {
  csilk_arena_chunk_t* head; /**< Head of chunk linked list. */
  size_t default_chunk_size; /**< Default size for new chunks. */
} csilk_arena_t;

/** @brief Create a new arena allocator. */
csilk_arena_t* csilk_arena_new(size_t default_chunk_size) {
  csilk_arena_t* arena = malloc(sizeof(csilk_arena_t));
  if (!arena) return NULL;
  arena->head = NULL;
  arena->default_chunk_size = default_chunk_size;
  return arena;
}

/** @brief Allocate aligned memory from the arena. */
void* csilk_arena_alloc(csilk_arena_t* arena, size_t size) {
  if (size > SIZE_MAX - 7) return NULL;

  size = (size + 7) & ~7;

  if (arena->head && (arena->head->size - arena->head->used) >= size) {
    void* ptr = arena->head->data + arena->head->used;
    arena->head->used += size;
    return ptr;
  }

  size_t chunk_size =
      size > arena->default_chunk_size ? size : arena->default_chunk_size;
  csilk_arena_chunk_t* chunk = malloc(sizeof(csilk_arena_chunk_t) + chunk_size);
  if (!chunk) return NULL;

  chunk->size = chunk_size;
  chunk->used = size;
  chunk->next = arena->head;
  arena->head = chunk;
  return chunk->data;
}

/** @brief Duplicate a string using the arena allocator. */
char* csilk_arena_strdup(csilk_arena_t* arena, const char* s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char* news = csilk_arena_alloc(arena, len + 1);
  if (news) {
    memcpy(news, s, len + 1);
  }
  return news;
}

/** @brief Free all arena chunks and the arena itself. */
void csilk_arena_free(csilk_arena_t* arena) {
  if (!arena) return;
  csilk_arena_chunk_t* curr = arena->head;
  while (curr) {
    csilk_arena_chunk_t* next = curr->next;
    free(curr);
    curr = next;
  }
  free(arena);
}

/** @brief Reset arena for reuse without freeing chunks. */
void csilk_arena_reset(csilk_arena_t* arena) {
  if (!arena) return;
  csilk_arena_chunk_t* curr = arena->head;
  while (curr) {
    curr->used = 0;
    curr = curr->next;
  }
}
