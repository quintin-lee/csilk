/**
 * @file af_xdp_zerocopy.c
 * @brief UMEM pool allocator for AF_XDP zero-copy I/O.
 *
 * Manages a single page-aligned UMEM buffer carved into fixed-size frames that
 * AF_XDP uses for zero-copy packet send/receive. Provides create/free lifecycle
 * for the pool; the aligned allocation avoids per-frame copies between user and
 * kernel space.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "af_xdp_internal.h"

/**
 * @brief Create a page-aligned AF_XDP UMEM frame pool.
 * @param[in] total_size Total UMEM size in bytes (must be > 0).
 * @param[in] frame_size Size of each frame in bytes (must be > 0).
 * @return A newly allocated pool with frame_count = total_size / frame_size
 *         frames, or NULL on invalid size or allocation failure.
 * @note The backing buffer is 4096-byte aligned via posix_memalign and zeroed.
 *       The caller frees it with csilk_xdp_umem_pool_free.
 */
csilk_xdp_umem_pool_t*
csilk_xdp_umem_pool_create(size_t total_size, uint32_t frame_size)
{
    if (total_size == 0 || frame_size == 0) {
        return NULL;
    }

    csilk_xdp_umem_pool_t* pool = (csilk_xdp_umem_pool_t*)calloc(1, sizeof(csilk_xdp_umem_pool_t));
    if (!pool) {
        return NULL;
    }

    pool->size = total_size;
    pool->frame_size = frame_size;
    pool->frame_count = (uint32_t)(total_size / frame_size);

    /* Allocate aligned UMEM page block */
    int res = posix_memalign(&pool->buffer, 4096, total_size);
    if (res != 0 || !pool->buffer) {
        free(pool);
        return NULL;
    }
    memset(pool->buffer, 0, total_size);

    return pool;
}

/**
 * @brief Destroy an AF_XDP UMEM pool and free its backing buffer.
 * @param[in] pool Pool previously returned by csilk_xdp_umem_pool_create.
 * @note No-op if pool is NULL. Frees the aligned UMEM buffer (if any) and the
 *       pool struct.
 */
void
csilk_xdp_umem_pool_free(csilk_xdp_umem_pool_t* pool)
{
    if (!pool) {
        return;
    }

    if (pool->buffer) {
        free(pool->buffer);
    }
    free(pool);
}
