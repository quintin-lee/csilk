/**
 * @file arena.c
 * @brief Arena allocator implementation for request-scoped memory.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk_internal.h"

/** @brief A single chunk in the arena linked list.
 *
 * Arena allocator manages memory in chunks. When a chunk is full, a new
 * chunk is allocated. All memory is freed at once when the arena is freed,
 * making it ideal for request-scoped allocations.
 */
typedef struct csilk_arena_chunk_s {
  struct csilk_arena_chunk_s* next; /**< Pointer to next chunk. */
  size_t size;                      /**< Total size of this chunk. */
  size_t used;                      /**< Bytes used in this chunk. */
  uint8_t data[];                   /**< Flexible array for chunk data. */
} csilk_arena_chunk_t;

/** @brief Arena allocator for request-scoped memory.
 *
 * Arena allocators allocate memory in large chunks and never free individual
 * allocations until the entire arena is freed. This eliminates fragmentation
 * and is ideal for per-request memory management where all allocations are
 * discarded together after processing.
 */
typedef struct csilk_arena_s {
  csilk_arena_chunk_t* head; /**< Head of chunk linked list. */
  size_t default_chunk_size; /**< Default size for new chunks. */
} csilk_arena_t;

/** @brief Create a new arena allocator.
 *
 * Allocates and initializes an arena memory manager. The arena allocates memory
 * in chunks of at least @p default_chunk_size bytes. Individual allocations
 * within the arena are never freed separately; instead, all memory is reclaimed
 * at once by calling csilk_arena_free() or csilk_arena_reset().
 *
 * @param default_chunk_size Minimum size in bytes for each new chunk.
 *                           Pass 0 to let the implementation choose a default.
 * @return Pointer to the new arena, or NULL on allocation failure.
 * @note The returned arena must be freed with csilk_arena_free().
 * @note This function is not thread-safe; each thread should use its own arena.
 */
csilk_arena_t* csilk_arena_new(size_t default_chunk_size) {
  csilk_arena_t* arena = malloc(sizeof(csilk_arena_t));
  if (!arena) return NULL;
  arena->head = NULL;
  arena->default_chunk_size = default_chunk_size;
  return arena;
}

/** @brief Allocate memory from the arena with 8-byte alignment.
 *
 * Returns memory from the current chunk if there is room; otherwise allocates
 * a new chunk large enough to satisfy the request. The returned memory is
 * zero-initialized only by virtue of being freshly allocated from the OS.
 *
 * @param arena The arena allocator (must not be NULL).
 * @param size  Number of bytes to allocate. The actual allocation is rounded
 *              up to the nearest multiple of 8 for alignment.
 * @return Pointer to the allocated block, or NULL on allocation failure.
 * @note The returned pointer must NOT be freed individually; all arena memory
 *       is reclaimed via csilk_arena_free() or csilk_arena_reset(). */
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

/** @brief Duplicate a null-terminated string using the arena allocator.
 *
 * Allocates enough arena memory for a copy of @p s, including the null
 * terminator, and copies the string contents.
 *
 * @param arena The arena allocator.
 * @param s     Source string to duplicate.
 * @return Pointer to the new string in arena memory, or NULL if @p s is NULL
 *         or on allocation failure.
 * @note The result is subject to the same lifetime rules as other arena
 *       allocations — it lives until the arena is freed or reset. */
char* csilk_arena_strdup(csilk_arena_t* arena, const char* s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char* news = csilk_arena_alloc(arena, len + 1);
  if (news) {
    memcpy(news, s, len + 1);
  }
  return news;
}

/** @brief Free all arena chunks and the arena structure itself.
 *
 * Walks the linked list of chunks, frees each one, then frees the arena
 * header. After this call the arena pointer is invalid and must not be used.
 *
 * @param arena The arena to destroy (may be NULL).
 * @note Safe to call with NULL — it is a no-op. */
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

/** @brief Reset arena for reuse without freeing underlying chunks.
 *
 * Sets the @c used counter to zero on every chunk in the chain, making all
 * arena memory available for new allocations. No system calls (malloc/free)
 * are performed, making this much cheaper than csilk_arena_free() + _new().
 *
 * @param arena The arena to reset (may be NULL).
 * @note Useful for request-scoped arenas that are recycled between requests.
 * @note Safe to call with NULL — it is a no-op. */
void csilk_arena_reset(csilk_arena_t* arena) {
  if (!arena) return;
  csilk_arena_chunk_t* curr = arena->head;
  while (curr) {
    curr->used = 0;
    curr = curr->next;
  }
}
