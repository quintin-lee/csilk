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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#endif

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"

/** @brief Maximum number of chunks to keep in the thread-local free list.
 * This limit prevents unbounded memory growth in long-running threads. */
enum { MAX_TLS_ARENA_CHUNKS = 16 };

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

/** @brief Thread-local free list of arena chunks for reuse. */
static _Thread_local csilk_arena_chunk_t* tls_chunk_free_list = nullptr;
static _Thread_local int                  tls_chunk_count = 0;

/** @brief Arena allocator for request-scoped memory.
 *
 * Arena allocators allocate memory in large chunks and never free individual
 * allocations until the entire arena is freed. This eliminates fragmentation
 * and is ideal for per-request memory management where all allocations are
 * discarded together after processing.
 *
 * @note This structure is padded to CSILK_CACHE_LINE_SIZE to prevent false
 *       sharing when multiple arena headers are allocated close to each other
 *       in memory.
 */
typedef struct csilk_arena_s {
    csilk_arena_chunk_t* head;               /**< Head of chunk linked list. */
    size_t               default_chunk_size; /**< Default size for new chunks. */
    int                  align_64;           /**< Non-zero to enable 64-byte alignment. */
    size_t               max_total_bytes;    /**< Maximum total bytes (0 = unlimited). */
    size_t               total_allocated;    /**< Total allocated bytes since last reset. */
    uint8_t
        _padding[CSILK_CACHE_LINE_SIZE - (2 * sizeof(size_t)) - sizeof(int) - 2 * sizeof(size_t)];
} csilk_arena_t;

/** @brief Thread-local cache structure with multiple size tiers.
 *  Each tier maintains an independent free list to avoid size mismatches. */
typedef struct {
    csilk_arena_chunk_t* chunks[CSILK_ARENA_TIER_COUNT]; /* Per-tier free lists */
    int                  count[CSILK_ARENA_TIER_COUNT];  /* Per-tier chunk counts */
    size_t               total_cached_bytes;             /* Total cached memory */
} arena_tls_cache_t;

/** @brief Cache line size (typically 64 bytes on modern CPUs).
 * Used for padding structures to prevent false sharing and improve
 * memory alignment. */

/** @brief Helper for cache-line aligned allocations.
 * Ensures the returned pointer starts at a 64-byte boundary.
 * Respects TEST_OOM for unit testing. */
static void*
arena_aligned_alloc(size_t size)
{
#ifdef TEST_OOM
    if (g_oom_fail_after >= 0 && g_oom_count >= g_oom_fail_after) {
        return nullptr;
    }
    g_oom_count++;
#endif

    void* ptr = nullptr;
    /* Guard (size + CLS - 1) against overflow. size is already bounded
     * by callers but this provides defense-in-depth. */
    if (size > SIZE_MAX - (CSILK_CACHE_LINE_SIZE - 1)) {
        return nullptr;
    }
    size_t aligned_size = (size + CSILK_CACHE_LINE_SIZE - 1) & ~(CSILK_CACHE_LINE_SIZE - 1);

#if defined(__APPLE__)
    mach_vm_address_t addr = 0;
    if (mach_vm_allocate(
            mach_task_self(), &addr, (mach_vm_size_t)aligned_size, VM_FLAGS_ANYWHERE) !=
        KERN_SUCCESS) {
        return nullptr;
    }
    ptr = (void*)addr;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    /* posix_memalign is preferred over C11 aligned_alloc because it is
     * available on a wider range of platforms (POSIX 2001), including
     * musl-based systems where aligned_alloc may not exist despite the
     * compiler advertising C11 conformance. */
    if (posix_memalign(&ptr, CSILK_CACHE_LINE_SIZE, aligned_size) != 0) {
        return nullptr;
    }
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    ptr = aligned_alloc(CSILK_CACHE_LINE_SIZE, aligned_size);
#else
    /* Fallback to standard malloc if no aligned allocation is available.
     The structure padding still provides some benefit by ensuring headers
     don't share a cache line if they are large enough. */
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

#ifdef DEBUG_ARENA
/** @brief Fill redzone bytes after allocation for overflow detection. */
static void
arena_fill_redzone(uint8_t* data, size_t size, size_t alloc_sz)
{
    /* Fill 16 bytes after user data with 0xBE pattern */
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

/** @brief Create a new arena allocator.
 *
 * Allocates and initializes an arena memory manager. The arena allocates memory
 * in chunks of at least @p default_chunk_size bytes. Individual allocations
 * within the arena are never freed separately; instead, all memory is reclaimed
 * at once by calling csilk_arena_free() or csilk_arena_reset().
 *
 * @param default_chunk_size Minimum size in bytes for each new chunk.
 *                           Pass 0 to let the implementation choose a default.
 * @return Pointer to the new arena, or nullptr on allocation failure.
 * @note The returned arena must be freed with csilk_arena_free().
 * @note This function is not thread-safe; each thread should use its own arena.
 */
csilk_arena_t*
csilk_arena_new(size_t default_chunk_size)
{
    csilk_arena_t* arena = arena_aligned_alloc(sizeof(csilk_arena_t));
    if (!arena) {
        return nullptr;
    }
    arena->head = nullptr;
    arena->default_chunk_size = default_chunk_size;
    arena->align_64 = 0;
    arena->max_total_bytes = 0; /* Unlimited by default */
    arena->total_allocated = 0; /* Reset counter on creation */
    return arena;
}

/** @brief Enable or disable 64-byte alignment for this arena.
 *
 * @param arena   The arena to configure.
 * @param enabled Non-zero to enable 64-byte alignment, zero for 8-byte.
 */
void
csilk_arena_set_alignment(csilk_arena_t* arena, int enabled)
{
    if (arena) {
        arena->align_64 = enabled;
    }
}

/**
 * @brief Set maximum total bytes for this arena.
 *
 * If set to 0 (default), the arena has no size limit and will grow
 * unbounded until manually freed. If set to a non-zero value, allocations
 * exceeding this limit will fail and return nullptr.
 *
 * @param arena    The arena to configure.
 * @param max_bytes Maximum total bytes (0 = unlimited).
 * @return 0 on success, -1 if arena is nullptr.
 * @note Setting a limit resets the total_allocated counter to 0.
 */
int csilk_arena_set_max_bytes(csilk_arena_t* arena, size_t max_bytes);

/**
 * @brief Set maximum total bytes for this arena.
 *
 * Implementation of csilk_arena_set_max_bytes.
 */
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

/** @brief Allocate memory from the arena with 8-byte alignment.
 *
 * Returns memory from the current chunk if there is room; otherwise allocates
 * a new chunk large enough to satisfy the request. The returned memory is
 * zero-initialized only by virtue of being freshly allocated from the OS.
 *
 * @param arena The arena allocator (may be nullptr — returns nullptr).
 * @param size  Number of bytes to allocate. The actual allocation is rounded
 *              up to the nearest multiple of 8 for alignment.
 * @return Pointer to the allocated block, or nullptr on allocation failure.
 * @note The returned pointer must NOT be freed individually; all arena memory
 *       is reclaimed via csilk_arena_free() or csilk_arena_reset(). */
void*
csilk_arena_alloc(csilk_arena_t* arena, size_t size)
{
    if (!arena) {
        return nullptr;
    }
    size_t alignment = arena->align_64 ? CSILK_CACHE_LINE_SIZE : 8;
    if (size > SIZE_MAX - (alignment - 1)) {
        return nullptr;
    }

    /* Zero-size allocation: return a non-null sentinel to prevent
     * degenerate (zero-capacity) chunks from being created. This
     * matches the spirit of malloc(0) returning a unique pointer. */
    if (size == 0) {
        return (void*)(uintptr_t)1;
    }

    size = (size + alignment - 1) & ~(alignment - 1);

#ifdef DEBUG_ARENA
    size_t alloc_size = size;
    size += ARENA_REDZONE_SIZE;
#endif

    if (arena->head) {
        size_t aligned_used = (arena->head->used + alignment - 1) & ~(alignment - 1);
        if ((arena->head->size - aligned_used) >= size) {
            void* ptr = arena->head->data + aligned_used;
#ifdef DEBUG_ARENA
            arena_fill_redzone(arena->head->data, arena->head->size, aligned_used + alloc_size);
#endif
            arena->head->used = aligned_used + size;
            return ptr;
        }
    }

    size_t chunk_size = size > arena->default_chunk_size ? size : arena->default_chunk_size;
    csilk_arena_chunk_t* chunk = nullptr;

    /* Try to reuse a chunk from the thread-local free list if it matches the
     standard size. This avoids expensive aligned_alloc syscalls in the
     hot path. */
    if (chunk_size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_free_list) {
        chunk = tls_chunk_free_list;
        tls_chunk_free_list = chunk->next;
        tls_chunk_count--;
    } else {
        /* Guard sizeof(chunk) + chunk_size against integer overflow.
         * In practice this is unreachable (requires allocating ~18 EB)
         * but provides formal correctness for all SIZE_MAX inputs. */
        if (chunk_size > SIZE_MAX - sizeof(csilk_arena_chunk_t)) {
            return nullptr;
        }

        /* Check if allocation would exceed max_total_bytes limit */
        if (arena->max_total_bytes > 0 &&
            (arena->total_allocated + chunk_size > arena->max_total_bytes)) {
            return nullptr;
        }

        chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
    }

    if (!chunk) {
        return nullptr;
    }

    chunk->size = chunk_size;
    chunk->used = size;
    chunk->next = arena->head;
    arena->head = chunk;
    arena->total_allocated += chunk_size;
    return chunk->data;
}

/** @brief Duplicate a null-terminated string using the arena allocator.
 *
 * Allocates enough arena memory for a copy of @p s, including the null
 * terminator, and copies the string contents.
 *
 * @param arena The arena allocator.
 * @param s     Source string to duplicate.
 * @return Pointer to the new string in arena memory, or nullptr if @p s is nullptr
 *         or on allocation failure.
 * @note The result is subject to the same lifetime rules as other arena
 *       allocations — it lives until the arena is freed or reset. */
char*
csilk_arena_strdup(csilk_arena_t* arena, const char* s)
{
    if (!s) {
        return nullptr;
    }
    size_t len = strlen(s);
    char*  news = csilk_arena_alloc(arena, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

/** @brief Duplicate @p n bytes of a string using the arena allocator.
 *
 * Allocates @p n + 1 bytes of arena memory, copies @p n bytes from @p s,
 * and adds a null terminator.
 *
 * @param arena The arena allocator.
 * @param s     Source string to duplicate.
 * @param n     Number of bytes to copy.
 * @return Pointer to the new string in arena memory, or nullptr if @p s is nullptr
 *         or on allocation failure. */
char*
csilk_arena_strndup(csilk_arena_t* arena, const char* s, size_t n)
{
    if (!s) {
        return nullptr;
    }
    char* news = csilk_arena_alloc(arena, n + 1);
    if (news) {
        memcpy(news, s, n);
        news[n] = '\0';
    }
    return news;
}

/** @brief Free all arena chunks and the arena structure itself.
 *
 * Walks the linked list of chunks, frees each one, then frees the arena
 * header. After this call the arena pointer is invalid and must not be used.
 *
 * @param arena The arena to destroy (may be nullptr).
 * @note Safe to call with nullptr — it is a no-op. */
void
csilk_arena_free(csilk_arena_t* arena)
{
    if (!arena) {
        return;
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

        /* Track total allocated bytes (excluding chunk headers).
         * MUST happen before the free below — after arena_aligned_free
         * the chunk memory is invalid and accessing curr->size is UB
         * (heap-use-after-free caught by ASAN/mach_vm_deallocate). */
        arena->total_allocated -= curr->size;

        /* Return standard-sized chunks to the thread-local free list if there is
        room. This speeds up subsequent allocations on the same thread. */
        if (curr->size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_count < MAX_TLS_ARENA_CHUNKS) {
            curr->next = tls_chunk_free_list;
            curr->used = 0;
            tls_chunk_free_list = curr;
            tls_chunk_count++;
        } else {
            arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
        }

        curr = next;
    }
    arena_aligned_free(arena, sizeof(csilk_arena_t));
}

/** @brief Reset arena for reuse without freeing underlying chunks.
 *
 * Sets the @c used counter to zero on every chunk in the chain, making all
 * arena memory available for new allocations. No system calls (malloc/free)
 * are performed, making this much cheaper than csilk_arena_free() + _new().
 *
 * @param arena The arena to reset (may be nullptr).
 * @note Useful for request-scoped arenas that are recycled between requests.
 * @note Safe to call with nullptr — it is a no-op. */
void
csilk_arena_reset(csilk_arena_t* arena)
{
    if (!arena) {
        return;
    }
    csilk_arena_chunk_t* curr = arena->head;
    while (curr) {
        curr->used = 0;
        curr = curr->next;
    }
    arena->total_allocated = 0;
}

/** @brief Flush the thread-local arena chunk free list.
 *
 * Frees all chunks cached in the calling thread's TLS free list. Call this
 * before the thread exits to prevent ASAN from reporting the cached chunks
 * as memory leaks when arenas were used on a non-main thread. */
void
csilk_arena_flush_free_list(void)
{
    csilk_arena_chunk_t* curr = tls_chunk_free_list;
    while (curr) {
        csilk_arena_chunk_t* next = curr->next;
        arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
        curr = next;
    }
    tls_chunk_free_list = nullptr;
    tls_chunk_count = 0;
}

/** @brief Initialize arena subsystem with automatic TLS cleanup.
 *
 * Creates a pthread key that automatically flushes the TLS chunk free list
 * when the thread exits. Safe to call multiple times; subsequent calls are
 * no-ops. */
static void
arena_tls_cleanup(void* unused)
{
    (void)unused;
    csilk_arena_flush_free_list();
}

static void
arena_init_tls_key(void)
{
    static pthread_key_t tls_key;
    pthread_key_create(&tls_key, arena_tls_cleanup);
}

void
csilk_arena_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, arena_init_tls_key);
}

#ifdef TEST_OOM
/** @brief Get the number of chunks currently in the thread-local free list.
 * Only available during testing. */
int
csilk_arena_get_tls_chunk_count(void)
{
    return tls_chunk_count;
}
#endif

/** @brief Get total allocated size and used bytes in the arena.
 *
 * Walks the chunk list and sums the total allocated size and total used bytes.
 *
 * @param arena      The arena to query (must not be nullptr).
 * @param[out] total_size Pointer to receive the total allocated size in bytes.
 * @param[out] total_used Pointer to receive the total used bytes in the arena.
 * @note Safe to call with nullptr pointers for total_size or total_used — they
 *       will simply be ignored. */
void
csilk_arena_get_stats(csilk_arena_t* arena, size_t* total_size, size_t* total_used)
{
    if (!arena || !total_size || !total_used) {
        return;
    }
    *total_size = 0;
    *total_used = 0;
    csilk_arena_chunk_t* curr = arena->head;
    while (curr) {
        *total_size += curr->size;
        *total_used += curr->used;
        curr = curr->next;
    }
}
