/**
 * @file wal_uring.h
 * @brief Asynchronous io_uring WAL batch writing and fsync engine for csilk.
 * @copyright MIT License
 */

#ifndef CSILK_WAL_URING_H
#define CSILK_WAL_URING_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_wal_ring_s csilk_wal_ring_t;

/**
 * @brief Checks whether the Linux io_uring asynchronous WAL backend is supported
 *        and operational in the current kernel environment.
 * @return 1 if supported, 0 if unavailable (fallback to POSIX).
 */
int csilk_wal_is_uring_supported(void);

/**
 * @brief Creates and initializes an asynchronous io_uring WAL ring.
 * @param queue_depth Ring submission queue capacity (e.g. 64, 128, 256).
 * @return Initialized WAL ring pointer, or NULL on allocation/init error.
 */
csilk_wal_ring_t* csilk_wal_ring_create(uint32_t queue_depth);

/**
 * @brief Destroys an asynchronous WAL ring and frees associated kernel mappings.
 * @param ring Ring instance.
 */
void csilk_wal_ring_destroy(csilk_wal_ring_t* ring);

/**
 * @brief Checks whether the WAL ring instance is backed by real Linux io_uring.
 * @param ring Ring instance.
 * @return 1 if backed by real io_uring, 0 if using POSIX fallback emulation.
 */
int csilk_wal_ring_is_real_uring(const csilk_wal_ring_t* ring);

/**
 * @brief Submits a vectorized WAL write request (IORING_OP_WRITEV).
 * @param ring WAL ring instance.
 * @param fd Target file descriptor.
 * @param iov Array of buffer vectors.
 * @param iovcnt Number of vectors in @p iov.
 * @param offset File offset to write to (-1 for current file offset).
 * @return 0 on success, negative value on failure.
 */
int csilk_wal_ring_submit_writev(
    csilk_wal_ring_t* ring, int fd, const struct iovec* iov, int iovcnt, off_t offset);

/**
 * @brief Submits an asynchronous WAL fsync request (IORING_OP_FSYNC).
 * @param ring WAL ring instance.
 * @param fd File descriptor to flush.
 * @param is_fdatasync Non-zero for fdatasync (data only), zero for full fsync.
 * @return 0 on success, negative value on failure.
 */
int csilk_wal_ring_submit_fsync(csilk_wal_ring_t* ring, int fd, int is_fdatasync);

/**
 * @brief Submits a linked writev + fsync request (IORING_OP_WRITEV linked to IORING_OP_FSYNC).
 *        The kernel executes the fsync immediately after the writev without returning to userspace.
 * @param ring WAL ring instance.
 * @param fd File descriptor.
 * @param iov Vector array.
 * @param iovcnt Number of vectors.
 * @param offset File offset (-1 for current offset).
 * @param is_fdatasync Non-zero for fdatasync.
 * @return 0 on success, negative value on failure.
 */
int csilk_wal_ring_submit_writev_fsync(csilk_wal_ring_t*   ring,
                                       int                 fd,
                                       const struct iovec* iov,
                                       int                 iovcnt,
                                       off_t               offset,
                                       int                 is_fdatasync);

/**
 * @brief Flushes all pending submissions to the kernel and waits for completion.
 * @param ring WAL ring instance.
 * @param min_complete Minimum number of completions to wait for (0 = submit only).
 * @return Number of completed operations, or negative value on failure.
 */
int csilk_wal_ring_flush(csilk_wal_ring_t* ring, uint32_t min_complete);

/**
 * @brief Returns total number of completed io_uring WAL operations.
 * @param ring WAL ring instance.
 * @return Total completions counter.
 */
uint64_t csilk_wal_ring_get_completed(const csilk_wal_ring_t* ring);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_WAL_URING_H */
