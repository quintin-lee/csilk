/**
 * @file arena.c
 * @brief Arena allocator implementation for request-scoped memory.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct gin_arena_chunk_s {
    struct gin_arena_chunk_s* next;
    size_t size;
    size_t used;
    uint8_t data[];
} gin_arena_chunk_t;

typedef struct gin_arena_s {
    gin_arena_chunk_t* head;
    size_t default_chunk_size;
} gin_arena_t;

gin_arena_t* gin_arena_new(size_t default_chunk_size) {
    gin_arena_t* arena = malloc(sizeof(gin_arena_t));
    if (!arena) return NULL;
    arena->head = NULL;
    arena->default_chunk_size = default_chunk_size;
    return arena;
}

void* gin_arena_alloc(gin_arena_t* arena, size_t size) {
    // Alignment to 8 bytes
    size = (size + 7) & ~7;

    if (arena->head && (arena->head->size - arena->head->used) >= size) {
        void* ptr = arena->head->data + arena->head->used;
        arena->head->used += size;
        return ptr;
    }

    size_t chunk_size = size > arena->default_chunk_size ? size : arena->default_chunk_size;
    gin_arena_chunk_t* chunk = malloc(sizeof(gin_arena_chunk_t) + chunk_size);
    if (!chunk) return NULL;

    chunk->size = chunk_size;
    chunk->used = size;
    chunk->next = arena->head;
    arena->head = chunk;
    return chunk->data;
}

char* gin_arena_strdup(gin_arena_t* arena, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* news = gin_arena_alloc(arena, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

void gin_arena_free(gin_arena_t* arena) {
    if (!arena) return;
    gin_arena_chunk_t* curr = arena->head;
    while (curr) {
        gin_arena_chunk_t* next = curr->next;
        free(curr);
        curr = next;
    }
    free(arena);
}
