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

csilk_uring_buf_ring_t*
csilk_uring_buf_ring_create(size_t num_bufs, size_t buf_size)
{
    if (num_bufs == 0 || buf_size == 0) {
        return nullptr;
    }

    csilk_uring_buf_ring_t* ring = malloc(sizeof(csilk_uring_buf_ring_t));
    if (!ring) {
        return nullptr;
    }

    ring->num_bufs = num_bufs;
    ring->buf_size = buf_size;
    ring->buffers = calloc(num_bufs, sizeof(void*));
    if (!ring->buffers) {
        free(ring);
        return nullptr;
    }

    /* Allocate page-aligned (4096-byte boundary) memory chunks for zero-copy kernel I/O */
    size_t align = 4096;
    size_t aligned_size = (buf_size + align - 1) & ~(align - 1);

    for (size_t i = 0; i < num_bufs; i++) {
        void* ptr = nullptr;
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
            return nullptr;
        }
        memset(ptr, 0, aligned_size);
        ring->buffers[i] = ptr;
    }

    return ring;
}

void*
csilk_uring_buf_ring_get(csilk_uring_buf_ring_t* ring, size_t index)
{
    if (!ring || !ring->buffers || index >= ring->num_bufs) {
        return nullptr;
    }
    return ring->buffers[index];
}

size_t
csilk_uring_buf_ring_get_buf_size(const csilk_uring_buf_ring_t* ring)
{
    return ring ? ring->buf_size : 0;
}

size_t
csilk_uring_buf_ring_get_num_bufs(const csilk_uring_buf_ring_t* ring)
{
    return ring ? ring->num_bufs : 0;
}

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
