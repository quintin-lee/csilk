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

/** @brief Opaque handle for io_uring Direct File Descriptor table. */
typedef struct csilk_uring_direct_files_s csilk_uring_direct_files_t;

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
 * @brief Check if the buffer ring is currently registered with a kernel io_uring.
 *
 * @param ring Buffer ring handle.
 * @return true if registered, false otherwise.
 */
bool csilk_uring_buf_ring_is_registered(const csilk_uring_buf_ring_t* ring);

/**
 * @brief Register buffer ring with an io_uring instance using io_uring_register_buffers.
 *
 * Pins and registers pre-allocated, page-aligned buffers with the Linux kernel
 * for zero-copy IORING_OP_READ_FIXED / IORING_OP_WRITE_FIXED operations.
 *
 * @param ring Buffer ring handle.
 * @param io_uring_ptr Pointer to struct io_uring (or csilk_io_loop_t / void* for portability).
 * @return 0 on success, negative errno on failure.
 */
int csilk_uring_buf_ring_register(csilk_uring_buf_ring_t* ring, void* io_uring_ptr);

/**
 * @brief Unregister a previously registered buffer ring from an io_uring instance.
 *
 * @param ring Buffer ring handle.
 * @param io_uring_ptr Pointer to struct io_uring (or csilk_io_loop_t / void*).
 * @return 0 on success, negative errno on failure.
 */
int csilk_uring_buf_ring_unregister(csilk_uring_buf_ring_t* ring, void* io_uring_ptr);

/**
 * @brief Free resources associated with a Registered Buffer Ring.
 *
 * @param ring Buffer ring handle.
 */
void csilk_uring_buf_ring_free(csilk_uring_buf_ring_t* ring);

/**
 * @brief Create a direct FD table for io_uring fixed file descriptor registration.
 *
 * @param max_files Maximum capacity of registered file descriptors.
 * @return Handle to direct files table, or NULL on allocation failure.
 */
csilk_uring_direct_files_t* csilk_uring_direct_files_create(size_t max_files);

/**
 * @brief Get total slot capacity of the direct files table.
 *
 * @param df Direct files handle.
 * @return Capacity in slots.
 */
size_t csilk_uring_direct_files_get_capacity(const csilk_uring_direct_files_t* df);

/**
 * @brief Get count of actively allocated slots in the direct files table.
 *
 * @param df Direct files handle.
 * @return Active slot count.
 */
size_t csilk_uring_direct_files_get_count(const csilk_uring_direct_files_t* df);

/**
 * @brief Check if the direct files table is registered with a kernel io_uring.
 *
 * @param df Direct files handle.
 * @return true if registered, false otherwise.
 */
bool csilk_uring_direct_files_is_registered(const csilk_uring_direct_files_t* df);

/**
 * @brief Register the file table with an io_uring instance using io_uring_register_files.
 *
 * @param df Direct files handle.
 * @param io_uring_ptr Pointer to struct io_uring.
 * @return 0 on success, negative errno on failure.
 */
int csilk_uring_direct_files_register(csilk_uring_direct_files_t* df, void* io_uring_ptr);

/**
 * @brief Unregister the file table from an io_uring instance.
 *
 * @param df Direct files handle.
 * @param io_uring_ptr Pointer to struct io_uring.
 * @return 0 on success, negative errno on failure.
 */
int csilk_uring_direct_files_unregister(csilk_uring_direct_files_t* df, void* io_uring_ptr);

/**
 * @brief Allocate a direct FD slot and assign a physical OS file descriptor.
 *
 * If the table is registered, updates kernel state dynamically via io_uring_register_files_update.
 *
 * @param df Direct files handle.
 * @param os_fd Physical OS file descriptor (socket or file).
 * @param io_uring_ptr Pointer to struct io_uring (optional for dynamic update, can be NULL).
 * @return Assigned direct FD slot index (>= 0), or -1 if full or on error.
 */
int
csilk_uring_direct_files_alloc_slot(csilk_uring_direct_files_t* df, int os_fd, void* io_uring_ptr);

/**
 * @brief Release a direct FD slot.
 *
 * @param df Direct files handle.
 * @param slot_idx Slot index returned by alloc_slot.
 * @param io_uring_ptr Pointer to struct io_uring (for dynamic update, can be NULL).
 * @return 0 on success, -1 on invalid slot.
 */
int csilk_uring_direct_files_free_slot(csilk_uring_direct_files_t* df,
                                       int                         slot_idx,
                                       void*                       io_uring_ptr);

/**
 * @brief Get the physical OS file descriptor assigned to a slot.
 *
 * @param df Direct files handle.
 * @param slot_idx Slot index.
 * @return The OS fd, or -1 if slot is empty or invalid.
 */
int csilk_uring_direct_files_get_fd(const csilk_uring_direct_files_t* df, int slot_idx);

/**
 * @brief Free resources associated with a direct files table.
 *
 * @param df Direct files handle.
 */
void csilk_uring_direct_files_free(csilk_uring_direct_files_t* df);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_URING_BUF_H */
