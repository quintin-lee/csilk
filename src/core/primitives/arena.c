/**
 * @file arena.c
 * @brief Arena (bump) allocator for request-scoped memory management.
 *
 * The arena allocator is the cornerstone of csilk's zero-freedown model.
 * Instead of freeing individual allocations (which causes fragmentation and
 * overhead), the arena allocates from large contiguous chunks and resets all
 * memory at once when the request completes.
 *
 * Benefits over malloc/free per allocation:
 *   - O(1) allocation (pointer bump, no free list search)
 *   - Minimal fast-path instructions (cached ptr/end in arena header)
 *   - Zero fragmentation within a chunk
 *   - Cache-friendly (sequential access pattern)
 *   - Perfect for request-scoped data (headers, params, storage values)
 *
 * Chunk structure:
 *   Each chunk is a linked-list node with a flexible array member (data[])
 *   containing the usable memory. When the current chunk runs out of space,
 *   a new chunk (at least default_chunk_size bytes) is prepended to the list.
 *   This means allocation always happens in the head chunk (most recently
 *   added), which typically has good cache residency.
 * @copyright MIT License
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#endif

#include "csilk/core/internal.h"

enum { MAX_TLS_CHUNKS_PER_TIER = 8 };

/** @brief CPU cache line size hint for arena alignment. */
enum { CSILK_CACHE_LINE_SIZE = 64 };

#ifdef DEBUG_ARENA
enum { ARENA_REDZONE_SIZE = 16 };
#endif

/** @brief A single chunk in the arena linked list.
 *
 * Arena allocator manages memory in chunks. When a chunk is full, a new
 * chunk is allocated. All memory is freed at once when the arena is freed,
 * making it ideal for request-scoped allocations.
 *
 * @note This structure is padded to CSILK_CACHE_LINE_SIZE to ensure that
 *       the data starts on a cache line boundary and to prevent false sharing
 *       between arenas assigned to different threads.
 */
typedef struct csilk_arena_chunk_s {
    struct csilk_arena_chunk_s* next;   /**< Pointer to next chunk. */
    size_t                      size;   /**< Total size of this chunk. */
    size_t                      used;   /**< Bytes used in this chunk. */
    uint8_t                     _padding[CSILK_CACHE_LINE_SIZE - (3 * sizeof(size_t))];
    uint8_t                     data[]; /**< Flexible array for chunk data. */
} csilk_arena_chunk_t;

/** @brief Helper to map chunk size to tier index (4KB, 16KB, 64KB). */
static inline int
arena_size_to_tier(size_t size)
{
    if (size == CSILK_DEFAULT_ARENA_SIZE) {
        return CSILK_ARENA_TIER_SMALL;  /* 4KB */
    }
    if (size == 16384) {
        return CSILK_ARENA_TIER_MEDIUM; /* 16KB */
    }
    if (size == 65536) {
        return CSILK_ARENA_TIER_LARGE;  /* 64KB */
    }
    return -1;
}

/** @brief Thread-local free lists of arena chunks partitioned by size tier. */
static _Thread_local csilk_arena_chunk_t* tls_tier_free_lists[CSILK_ARENA_TIER_COUNT] = {NULL};
static _Thread_local int                  tls_tier_counts[CSILK_ARENA_TIER_COUNT] = {0};

static inline int
arena_get_total_tls_chunk_count(void)
{
    int total = 0;
    for (int i = 0; i < CSILK_ARENA_TIER_COUNT; i++) {
        total += tls_tier_counts[i];
    }
    return total;
}

/** @brief Arena allocator for request-scoped memory.
 *
 * Direct fast-path pointers `ptr` and `end` are stored on the very first
 * cache line to minimize loads and memory dereferences during allocation.
 *
 * @note This structure is padded to CSILK_CACHE_LINE_SIZE to prevent false
 *       sharing when multiple arena headers are allocated close to each other
 *       in memory.
 */
typedef struct csilk_arena_s {
    uint8_t*             ptr;                /**< Current bump pointer in active chunk. */
    uint8_t*             end;                /**< End boundary pointer of active chunk. */
    csilk_arena_chunk_t* head;               /**< Head of chunk linked list. */
    size_t               default_chunk_size; /**< Default size for new chunks. */
    size_t               max_total_bytes;    /**< Maximum total bytes (0 = unlimited). */
    size_t               total_allocated;    /**< Total allocated bytes since last reset. */
    int                  align_64;           /**< Non-zero to enable 64-byte alignment. */
    uint8_t
        _padding[CSILK_CACHE_LINE_SIZE - (3 * sizeof(void*)) - (3 * sizeof(size_t)) - sizeof(int)];
} csilk_arena_t;

/** @brief Helper for cache-line aligned allocations.
 * Ensures the returned pointer starts at a 64-byte boundary.
 * Respects TEST_OOM for unit testing. */
static void*
arena_aligned_alloc(size_t size)
{
#ifdef TEST_OOM
    if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) {
        return NULL;
    }
    g_oom_count++;
#endif

    void* ptr = NULL;
    /* Guard (size + CLS - 1) against overflow. */
    if (size > SIZE_MAX - (CSILK_CACHE_LINE_SIZE - 1)) {
        return NULL;
    }
    size_t aligned_size = (size + CSILK_CACHE_LINE_SIZE - 1) & ~(CSILK_CACHE_LINE_SIZE - 1);

#if defined(__APPLE__)
    mach_vm_address_t addr = 0;
    if (mach_vm_allocate(
            mach_task_self(), &addr, (mach_vm_size_t)aligned_size, VM_FLAGS_ANYWHERE) !=
        KERN_SUCCESS) {
        return NULL;
    }
    ptr = (void*)addr;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    if (posix_memalign(&ptr, CSILK_CACHE_LINE_SIZE, aligned_size) != 0) {
        return NULL;
    }
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    ptr = aligned_alloc(CSILK_CACHE_LINE_SIZE, aligned_size);
#else
    ptr = malloc(aligned_size);
#endif
    return ptr;
}

/** @brief Helper for freeing cache-line aligned allocations. */
static void
arena_aligned_free(void* ptr, size_t size)
{
    if (!ptr) {
        return;
    }
    size_t aligned_size = (size + CSILK_CACHE_LINE_SIZE - 1) & ~(CSILK_CACHE_LINE_SIZE - 1);

#if defined(__APPLE__)
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)ptr, (mach_vm_size_t)aligned_size);
#else
    free(ptr);
#endif
}

/** @brief Flush the thread-local arena chunk free list.
 *
 * Frees all chunks cached in the calling thread's TLS free list. Call this
 * before the thread exits to prevent ASAN from reporting the cached chunks
 * as memory leaks when arenas were used on a non-main thread. */
void
csilk_arena_flush_free_list(void)
{
    for (int t = 0; t < CSILK_ARENA_TIER_COUNT; t++) {
        csilk_arena_chunk_t* curr = tls_tier_free_lists[t];
        while (curr) {
            csilk_arena_chunk_t* next = curr->next;
            arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
            curr = next;
        }
        tls_tier_free_lists[t] = NULL;
        tls_tier_counts[t] = 0;
    }
}

static pthread_key_t g_arena_tls_key;

/** @brief Initialize arena subsystem with automatic TLS cleanup. */
static void
arena_tls_cleanup(void* unused)
{
    (void)unused;
    csilk_arena_flush_free_list();
}

static void
arena_init_tls_key(void)
{
    pthread_key_create(&g_arena_tls_key, arena_tls_cleanup);
}

/**
 * @brief Initialize the arena subsystem.
 *
 * Registers the thread-local arena chunk cleanup handler exactly once per
 * process via pthread_once. Safe to call multiple times.
 */
void
csilk_arena_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, arena_init_tls_key);
}

static inline void
arena_ensure_tls_cleanup_registered(void)
{
    csilk_arena_init();
    if (pthread_getspecific(g_arena_tls_key) == NULL) {
        pthread_setspecific(g_arena_tls_key, (void*)1);
    }
}

#ifdef DEBUG_ARENA
/** @brief Fill redzone bytes after allocation for overflow detection. */
static void
arena_fill_redzone(uint8_t* data, size_t size, size_t alloc_sz)
{
    const uint8_t pattern = 0xBE;
    for (size_t i = 0; i < ARENA_REDZONE_SIZE; i++) {
        data[alloc_sz + i] = pattern;
    }
}

/** @brief Verify redzone bytes haven't been corrupted. */
static int
arena_check_redzone(uint8_t* data, size_t alloc_sz)
{
    const uint8_t pattern = 0xBE;
    for (size_t i = 0; i < ARENA_REDZONE_SIZE; i++) {
        if (data[alloc_sz + i] != pattern) {
            return 0; /* Corrupted */
        }
    }
    return 1;         /* OK */
}
#endif

/** @brief Create a new arena allocator. */
csilk_arena_t*
csilk_arena_new(size_t default_chunk_size)
{
    csilk_arena_t* arena = arena_aligned_alloc(sizeof(csilk_arena_t));
    if (!arena) {
        return NULL;
    }
    arena->ptr = NULL;
    arena->end = NULL;
    arena->head = NULL;
    arena->default_chunk_size =
        default_chunk_size > 0 ? default_chunk_size : CSILK_DEFAULT_ARENA_SIZE;
    arena->align_64 = 0;
    arena->max_total_bytes = 0; /* Unlimited by default */
    arena->total_allocated = 0; /* Reset counter on creation */
    return arena;
}

/** @brief Enable or disable 64-byte alignment for this arena. */
void
csilk_arena_set_alignment(csilk_arena_t* arena, int enabled)
{
    if (arena) {
        arena->align_64 = enabled;
    }
}

/** @brief Set maximum total bytes for this arena. */
int
csilk_arena_set_max_bytes(csilk_arena_t* arena, size_t max_bytes)
{
    if (!arena) {
        return -1;
    }
    arena->max_total_bytes = max_bytes;
    arena->total_allocated = 0; /* Reset counter when limit is set */
    return 0;
}

/** @brief Slow path for arena chunk expansion and alignment handling. */
static void*
arena_alloc_slow(csilk_arena_t* arena, size_t size, size_t alignment)
{
    if (size > SIZE_MAX - (alignment - 1)) {
        return NULL;
    }
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);

    if (arena->head && arena->ptr) {
        arena->head->used = (size_t)(arena->ptr - arena->head->data);
    }

    size_t chunk_size =
        aligned_size > arena->default_chunk_size ? aligned_size : arena->default_chunk_size;
    /* Guard aligned_size + alignment against overflow before using it. */
    if (alignment > 8 && chunk_size < aligned_size &&
        chunk_size <= SIZE_MAX - alignment) {
        chunk_size = aligned_size + alignment;
    }

    /* Guard sizeof(chunk) + chunk_size against integer overflow */
    if (chunk_size > SIZE_MAX - sizeof(csilk_arena_chunk_t)) {
        return NULL;
    }

    /* Check if allocation would exceed max_total_bytes limit.
     * Use subtraction to avoid overflow on total_allocated + chunk_size. */
    if (arena->max_total_bytes > 0 && chunk_size > arena->max_total_bytes - arena->total_allocated) {
        return NULL;
    }

    /* Try to reuse a chunk from the thread-local free list */
    csilk_arena_chunk_t* chunk = NULL;
    int                  tier = arena_size_to_tier(chunk_size);
    if (tier >= 0 && tls_tier_free_lists[tier]) {
        chunk = tls_tier_free_lists[tier];
        tls_tier_free_lists[tier] = chunk->next;
        tls_tier_counts[tier]--;
    } else {
        chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
    }

    if (!chunk) {
        return NULL;
    }

    chunk->size = chunk_size;
    chunk->next = arena->head;
    arena->head = chunk;
    /* Guard total_allocated against overflow. */
    if (chunk_size > SIZE_MAX - arena->total_allocated) {
        /* Roll back: unsplice chunk and free it. */
        arena->head = chunk->next;
        if (tier >= 0 && tls_tier_counts[tier] < MAX_TLS_CHUNKS_PER_TIER &&
            arena_get_total_tls_chunk_count() < CSILK_MAX_TLS_CHUNKS) {
            chunk->next = tls_tier_free_lists[tier];
            tls_tier_free_lists[tier] = chunk;
            tls_tier_counts[tier]++;
        } else {
            arena_aligned_free(chunk, chunk->size + sizeof(csilk_arena_chunk_t));
        }
        return NULL;
    }
    arena->total_allocated += chunk_size;

    uintptr_t base = (uintptr_t)chunk->data;
    uintptr_t aligned_base = (base + alignment - 1) & ~(alignment - 1);
    arena->ptr = (uint8_t*)(aligned_base + aligned_size);
    arena->end = chunk->data + chunk_size;
    chunk->used = (size_t)(arena->ptr - chunk->data);

    return (void*)aligned_base;
}

#ifdef DEBUG_ARENA
/** @brief Debug allocation path with redzone verification. */
static void*
arena_alloc_debug(csilk_arena_t* arena, size_t size)
{
    if (!arena || size == 0) {
        return size == 0 ? (void*)(uintptr_t)1 : NULL;
    }
    size_t alignment = arena->align_64 ? CSILK_CACHE_LINE_SIZE : 8;
    if (size > SIZE_MAX - (alignment - 1) - ARENA_REDZONE_SIZE) {
        return NULL;
    }
    size_t alloc_sz = (size + alignment - 1) & ~(alignment - 1);
    size_t full_sz = alloc_sz + ARENA_REDZONE_SIZE;

    if (arena->head) {
        size_t used = (size_t)(arena->ptr - arena->head->data);
        size_t aligned_used = (used + alignment - 1) & ~(alignment - 1);
        if (arena->head->size >= aligned_used && (arena->head->size - aligned_used) >= full_sz) {
            uint8_t* ptr = arena->head->data + aligned_used;
            arena_fill_redzone(arena->head->data, arena->head->size, aligned_used + alloc_sz);
            arena->ptr = ptr + full_sz;
            arena->head->used = (size_t)(arena->ptr - arena->head->data);
            return ptr;
        }
    }

    void* ptr = arena_alloc_slow(arena, full_sz, alignment);
    if (ptr) {
        arena_fill_redzone((uint8_t*)ptr, full_sz, alloc_sz);
    }
    return ptr;
}
#endif

/** @brief Allocate memory from the arena with ultra-low latency fast path.
 *
 * Returns memory from the current chunk if there is room; otherwise delegates
 * to arena_alloc_slow().
 *
 * @param arena The arena allocator (may be NULL — returns NULL).
 * @param size  Number of bytes to allocate.
 * @return Pointer to the allocated block, or NULL on allocation failure.
 */
void*
csilk_arena_alloc(csilk_arena_t* arena, size_t size)
{
#ifdef DEBUG_ARENA
    return arena_alloc_debug(arena, size);
#else
    if (__builtin_expect(!arena || size == 0, 0)) {
        return size == 0 ? (void*)(uintptr_t)1 : NULL;
    }

    /* Fast path: 8-byte aligned (default) */
    if (__builtin_expect(arena->align_64 == 0, 1)) {
        if (__builtin_expect(size > SIZE_MAX - 7, 0)) {
            return NULL;
        }
        size_t       aligned_size = (size + 7) & ~7ULL;
        uintptr_t    cur     = (uintptr_t)arena->ptr;
        uintptr_t    next    = cur + aligned_size;

        if (__builtin_expect(next <= (uintptr_t)arena->end && cur != 0, 1)) {
            arena->ptr = (uint8_t*)next;
            return (void*)cur;
        }
        return arena_alloc_slow(arena, size, 8);
    }

    /* 64-byte aligned path */
    if (__builtin_expect(size > SIZE_MAX - 63, 0)) {
        return NULL;
    }
    size_t    aligned_size = (size + 63) & ~63ULL;
    uintptr_t cur = (uintptr_t)arena->ptr;
    uintptr_t aligned_cur = (cur + 63) & ~63ULL;
    uint8_t*  next = (uint8_t*)aligned_cur + aligned_size;

    if (__builtin_expect(next <= arena->end && arena->ptr != NULL, 1)) {
        arena->ptr = next;
        return (void*)aligned_cur;
    }
    return arena_alloc_slow(arena, size, 64);
#endif
}

/** @brief Allocate zero-initialised memory for an array from an arena. */
void*
csilk_arena_calloc(csilk_arena_t* arena, size_t count, size_t size)
{
    if (count > 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    size_t total = count * size;
    void*  ptr = csilk_arena_alloc(arena, total);
    if (ptr && total > 0) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/** @brief Duplicate a null-terminated string using the arena allocator. */
char*
csilk_arena_strdup(csilk_arena_t* arena, const char* s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char*  news = csilk_arena_alloc(arena, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

/** @brief Duplicate @p n bytes of a string using the arena allocator. */
char*
csilk_arena_strndup(csilk_arena_t* arena, const char* s, size_t n)
{
    if (!s) {
        return NULL;
    }
    char* news = csilk_arena_alloc(arena, n + 1);
    if (news) {
        memcpy(news, s, n);
        news[n] = '\0';
    }
    return news;
}

/** @brief Free all arena chunks and the arena structure itself. */
void
csilk_arena_free(csilk_arena_t* arena)
{
    if (!arena) {
        return;
    }
    if (arena->head) {
        arena->head->used = (size_t)(arena->ptr - arena->head->data);
    }
    csilk_arena_chunk_t* curr = arena->head;
    while (curr) {
        csilk_arena_chunk_t* next = curr->next;
#ifdef DEBUG_ARENA
        if (curr->used > ARENA_REDZONE_SIZE && curr->used <= curr->size) {
            if (!arena_check_redzone(curr->data, curr->used - ARENA_REDZONE_SIZE)) {
                fprintf(stderr,
                        "ARENA REDZONE CORRUPTED: chunk %p used=%zu size=%zu\n",
                        (void*)curr,
                        curr->used,
                        curr->size);
                abort();
            }
        }
#endif
        /* Guard total_allocated against underflow. */
        if (curr->size > arena->total_allocated) {
            arena->total_allocated = 0;
        } else {
            arena->total_allocated -= curr->size;
        }

        /* Return tiered chunks to the thread-local free list if there is room. */
        int tier = arena_size_to_tier(curr->size);
        if (tier >= 0 && tls_tier_counts[tier] < MAX_TLS_CHUNKS_PER_TIER &&
            arena_get_total_tls_chunk_count() < CSILK_MAX_TLS_CHUNKS) {
            arena_ensure_tls_cleanup_registered();
            curr->next = tls_tier_free_lists[tier];
            curr->used = 0;
            tls_tier_free_lists[tier] = curr;
            tls_tier_counts[tier]++;
        } else {
            arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
        }

        curr = next;
    }
    arena_aligned_free(arena, sizeof(csilk_arena_t));
}

/** @brief Reset arena for reuse without freeing underlying chunks. */
void
csilk_arena_reset(csilk_arena_t* arena)
{
    if (!arena) {
        return;
    }
    csilk_arena_chunk_t* head = arena->head;
    if (head) {
        head->used = 0;
        arena->ptr = head->data;
        arena->end = head->data + head->size;
        csilk_arena_chunk_t* curr = head->next;
        head->next = NULL;
        while (curr) {
            csilk_arena_chunk_t* next = curr->next;
            int                  tier = arena_size_to_tier(curr->size);
            if (tier >= 0 && tls_tier_counts[tier] < MAX_TLS_CHUNKS_PER_TIER &&
                arena_get_total_tls_chunk_count() < CSILK_MAX_TLS_CHUNKS) {
                arena_ensure_tls_cleanup_registered();
                curr->next = tls_tier_free_lists[tier];
                curr->used = 0;
                tls_tier_free_lists[tier] = curr;
                tls_tier_counts[tier]++;
            } else {
                arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
            }
            curr = next;
        }
        arena->total_allocated = head->size;
    } else {
        arena->ptr = NULL;
        arena->end = NULL;
        arena->total_allocated = 0;
    }
}

#ifdef TEST_OOM
/** @brief Get the number of chunks currently in the thread-local free list. */
int
csilk_arena_get_tls_chunk_count(void)
{
    return arena_get_total_tls_chunk_count();
}
#endif

/** @brief Get total allocated size and used bytes in the arena. */
void
csilk_arena_get_stats(csilk_arena_t* arena, size_t* total_size, size_t* total_used)
{
    if (!arena || !total_size || !total_used) {
        return;
    }
    *total_size = 0;
    *total_used = 0;
    if (arena->head) {
        arena->head->used = (size_t)(arena->ptr - arena->head->data);
    }
    csilk_arena_chunk_t* curr = arena->head;
    while (curr) {
        /* Guard against overflow when summing chunk sizes. */
        if (curr->size > SIZE_MAX - *total_size) {
            *total_size = SIZE_MAX;
        } else {
            *total_size += curr->size;
        }
        if (curr->used > SIZE_MAX - *total_used) {
            *total_used = SIZE_MAX;
        } else {
            *total_used += curr->used;
        }
        curr = curr->next;
    }
}

int
csilk_arena_contains(const csilk_arena_t* arena, const void* ptr)
{
    if (!arena || !ptr) {
        return 0;
    }
    const uint8_t* p = (const uint8_t*)ptr;
    for (csilk_arena_chunk_t* ch = arena->head; ch; ch = ch->next) {
        const uint8_t* start = (const uint8_t*)ch->data;
        const uint8_t* end = start + ch->size;
        if (p >= start && p < end) {
            return 1;
        }
    }
    return 0;
}
