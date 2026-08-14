/**
 * @file uring_buf.c
 * @brief io_uring Hardware-Level Registered Buffers & Page-Aligned Ring implementation.
 */

#include "csilk/core/uring_buf.h"
#include "csilk/csilk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#include <unistd.h>
#endif

struct csilk_uring_buf_ring_s {
    size_t num_bufs;
    size_t buf_size;
    void** buffers;
};

/**
 * @brief Create a ring of page-aligned registered buffers for zero-copy I/O.
 * @param[in] num_bufs Number of buffers in the ring (must be > 0).
 * @param[in] buf_size Size of each buffer in bytes (must be > 0).
 * @return A newly allocated ring, or NULL on invalid args or allocation failure.
 * @note Each buffer is 4096-byte aligned (via posix_memalign when available) and
 *       zeroed; on partial failure all allocated buffers are freed first.
 */
csilk_uring_buf_ring_t*
csilk_uring_buf_ring_create(size_t num_bufs, size_t buf_size)
{
    if (num_bufs == 0 || buf_size == 0) {
        return NULL;
    }

    csilk_uring_buf_ring_t* ring = malloc(sizeof(csilk_uring_buf_ring_t));
    if (!ring) {
        return NULL;
    }

    ring->num_bufs = num_bufs;
    ring->buf_size = buf_size;
    ring->buffers = calloc(num_bufs, sizeof(void*));
    if (!ring->buffers) {
        free(ring);
        return NULL;
    }

    /* Allocate page-aligned (4096-byte boundary) memory chunks for zero-copy kernel I/O */
    size_t align = 4096;
    size_t aligned_size = (buf_size + align - 1) & ~(align - 1);

    for (size_t i = 0; i < num_bufs; i++) {
        void* ptr = NULL;
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L || defined(__APPLE__) ||                \
    defined(__linux__)
        if (posix_memalign(&ptr, align, aligned_size) != 0) {
            ptr = malloc(aligned_size);
        }
#else
        ptr = malloc(aligned_size);
#endif
        if (!ptr) {
            /* Cleanup on partial allocation failure */
            for (size_t j = 0; j < i; j++) {
                free(ring->buffers[j]);
            }
            free(ring->buffers);
            free(ring);
            return NULL;
        }
        memset(ptr, 0, aligned_size);
        ring->buffers[i] = ptr;
    }

    return ring;
}

/**
 * @brief Retrieve a buffer from the ring by index.
 * @param[in] ring  Buffer ring (validated non-NULL with a buffer array).
 * @param[in] index Index of the requested buffer.
 * @return Pointer to the buffer, or NULL if ring is NULL or index is out of range.
 */
void*
csilk_uring_buf_ring_get(csilk_uring_buf_ring_t* ring, size_t index)
{
    if (!ring || !ring->buffers || index >= ring->num_bufs) {
        return NULL;
    }
    return ring->buffers[index];
}

/**
 * @brief Get the configured size of each buffer in the ring.
 * @param[in] ring Buffer ring (may be NULL).
 * @return Buffer size in bytes, or 0 if ring is NULL.
 */
size_t
csilk_uring_buf_ring_get_buf_size(const csilk_uring_buf_ring_t* ring)
{
    return ring ? ring->buf_size : 0;
}

/**
 * @brief Get the number of buffers in the ring.
 * @param[in] ring Buffer ring (may be NULL).
 * @return Number of buffers, or 0 if ring is NULL.
 */
size_t
csilk_uring_buf_ring_get_num_bufs(const csilk_uring_buf_ring_t* ring)
{
    return ring ? ring->num_bufs : 0;
}

/**
 * @brief Free a buffer ring and all its backing buffers.
 * @param[in] ring Ring to free (no-op if NULL).
 * @note Frees each buffer, the buffer pointer array, and the ring struct.
 */
void
csilk_uring_buf_ring_free(csilk_uring_buf_ring_t* ring)
{
    if (!ring) {
        return;
    }
    if (ring->buffers) {
        for (size_t i = 0; i < ring->num_bufs; i++) {
            if (ring->buffers[i]) {
                free(ring->buffers[i]);
            }
        }
        free(ring->buffers);
    }
    free(ring);
}
