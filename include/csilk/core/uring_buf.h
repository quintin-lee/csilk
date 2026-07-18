/**
 * @file uring_buf.h
 * @brief io_uring Hardware-Level Registered Buffers & 4096-Byte Page-Aligned Buffer Ring.
 */

#ifndef CSILK_CORE_URING_BUF_H
#define CSILK_CORE_URING_BUF_H

#include "csilk/csilk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for io_uring Registered Buffer Ring. */
typedef struct csilk_uring_buf_ring_s csilk_uring_buf_ring_t;

/**
 * @brief Create a new page-aligned (4096-byte boundary) Registered Buffer Ring.
 *
 * Allocates buffer memory aligned to page boundaries (4096 bytes) to eliminate
 * kernel memory mapping overheads when using io_uring_register_buffers / IORING_REGISTER_BUF_RING.
 *
 * @param num_bufs Number of buffer slots to allocate.
 * @param buf_size Capacity of each buffer slot in bytes.
 * @return Handle to buffer ring, or NULL on allocation failure.
 */
csilk_uring_buf_ring_t* csilk_uring_buf_ring_create(size_t num_bufs, size_t buf_size);

/**
 * @brief Get a pointer to a specific buffer slot in the ring.
 *
 * @param ring Buffer ring handle.
 * @param index 0-based buffer slot index (must be < num_bufs).
 * @return Pointer to page-aligned buffer slot, or NULL if index out of bounds.
 */
void* csilk_uring_buf_ring_get(csilk_uring_buf_ring_t* ring, size_t index);

/**
 * @brief Get buffer slot size in bytes.
 *
 * @param ring Buffer ring handle.
 * @return Size of individual buffer slot in bytes.
 */
size_t csilk_uring_buf_ring_get_buf_size(const csilk_uring_buf_ring_t* ring);

/**
 * @brief Get total number of buffer slots in ring.
 *
 * @param ring Buffer ring handle.
 * @return Total number of buffer slots.
 */
size_t csilk_uring_buf_ring_get_num_bufs(const csilk_uring_buf_ring_t* ring);

/**
 * @brief Free resources associated with a Registered Buffer Ring.
 *
 * @param ring Buffer ring handle.
 */
void csilk_uring_buf_ring_free(csilk_uring_buf_ring_t* ring);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_URING_BUF_H */
