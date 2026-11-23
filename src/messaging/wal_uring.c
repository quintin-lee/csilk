/**
 * @file wal_uring.c
 * @brief Asynchronous io_uring WAL batch writing and fsync engine implementation.
 * @copyright MIT License
 */

#include "csilk/messaging/wal_uring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#endif

/* --- File-scope static variable declarations --- */

static int g_uring_tested = 0;
static int g_uring_available = 0;

/* --- Internal Structure --- */

struct csilk_wal_ring_s {
    int      ring_fd;
    uint32_t queue_depth;
    int      is_real_uring;

#if defined(__linux__)
    struct io_uring_sqe* sqes;
    uint32_t*            sq_khead;
    uint32_t*            sq_ktail;
    uint32_t*            sq_kmask;
    uint32_t*            sq_kflags;
    uint32_t*            sq_array;

    struct io_uring_cqe* cqes;
    uint32_t*            cq_khead;
    uint32_t*            cq_ktail;
    uint32_t*            cq_kmask;

    void*  sq_ptr;
    size_t sq_size;
    void*  cq_ptr;
    size_t cq_size;
    void*  sqes_ptr;
    size_t sqes_size;

    uint32_t sq_tail;
#endif

    uint32_t pending_submits;
    uint64_t completed_count;
};

/* --- Public API --- */

int
csilk_wal_is_uring_supported(void)
{
    if (g_uring_tested) {
        return g_uring_available;
    }

#if defined(__linux__)
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    long fd = syscall(__NR_io_uring_setup, 1, &params);
    if (fd >= 0) {
        close((int)fd);
        g_uring_available = 1;
    } else {
        g_uring_available = 0;
    }
#else
    g_uring_available = 0;
#endif

    g_uring_tested = 1;
    return g_uring_available;
}

csilk_wal_ring_t*
csilk_wal_ring_create(uint32_t queue_depth)
{
    if (queue_depth == 0) {
        queue_depth = 128;
    }

    csilk_wal_ring_t* ring = calloc(1, sizeof(csilk_wal_ring_t));
    if (!ring) {
        return NULL;
    }

    ring->ring_fd = -1;
    ring->queue_depth = queue_depth;
    ring->is_real_uring = 0;

#if defined(__linux__)
    if (csilk_wal_is_uring_supported()) {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));

        long fd = syscall(__NR_io_uring_setup, queue_depth, &params);
        if (fd >= 0) {
            ring->ring_fd = (int)fd;

            size_t sq_size = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
            size_t cq_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

            if (params.features & IORING_FEAT_SINGLE_MMAP) {
                if (cq_size > sq_size) {
                    sq_size = cq_size;
                }
                cq_size = sq_size;
            }

            void* sq_ptr = mmap(0,
                                sq_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE,
                                ring->ring_fd,
                                IORING_OFF_SQ_RING);
            void* cq_ptr = (params.features & IORING_FEAT_SINGLE_MMAP)
                               ? sq_ptr
                               : mmap(0,
                                      cq_size,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_POPULATE,
                                      ring->ring_fd,
                                      IORING_OFF_CQ_RING);

            size_t sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
            void*  sqes_ptr = mmap(0,
                                  sqes_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE,
                                  ring->ring_fd,
                                  IORING_OFF_SQES);

            if (sq_ptr != MAP_FAILED && cq_ptr != MAP_FAILED && sqes_ptr != MAP_FAILED) {
                ring->sq_ptr = sq_ptr;
                ring->sq_size = sq_size;
                ring->cq_ptr = cq_ptr;
                ring->cq_size = cq_size;
                ring->sqes_ptr = sqes_ptr;
                ring->sqes_size = sqes_size;

                ring->sq_khead = (uint32_t*)((uint8_t*)sq_ptr + params.sq_off.head);
                ring->sq_ktail = (uint32_t*)((uint8_t*)sq_ptr + params.sq_off.tail);
                ring->sq_kmask = (uint32_t*)((uint8_t*)sq_ptr + params.sq_off.ring_mask);
                ring->sq_kflags = (uint32_t*)((uint8_t*)sq_ptr + params.sq_off.flags);
                ring->sq_array = (uint32_t*)((uint8_t*)sq_ptr + params.sq_off.array);

                ring->cq_khead = (uint32_t*)((uint8_t*)cq_ptr + params.cq_off.head);
                ring->cq_ktail = (uint32_t*)((uint8_t*)cq_ptr + params.cq_off.tail);
                ring->cq_kmask = (uint32_t*)((uint8_t*)cq_ptr + params.cq_off.ring_mask);
                ring->cqes = (struct io_uring_cqe*)((uint8_t*)cq_ptr + params.cq_off.cqes);

                ring->sqes = (struct io_uring_sqe*)sqes_ptr;
                ring->sq_tail = 0;
                ring->is_real_uring = 1;
            } else {
                if (sq_ptr != MAP_FAILED) {
                    munmap(sq_ptr, sq_size);
                }
                if (cq_ptr != MAP_FAILED && cq_ptr != sq_ptr) {
                    munmap(cq_ptr, cq_size);
                }
                if (sqes_ptr != MAP_FAILED) {
                    munmap(sqes_ptr, sqes_size);
                }
                close(ring->ring_fd);
                ring->ring_fd = -1;
            }
        }
    }
#endif

    return ring;
}

void
csilk_wal_ring_destroy(csilk_wal_ring_t* ring)
{
    if (!ring) {
        return;
    }

#if defined(__linux__)
    if (ring->is_real_uring) {
        if (ring->sq_ptr && ring->sq_ptr != MAP_FAILED) {
            munmap(ring->sq_ptr, ring->sq_size);
        }
        if (ring->cq_ptr && ring->cq_ptr != ring->sq_ptr && ring->cq_ptr != MAP_FAILED) {
            munmap(ring->cq_ptr, ring->cq_size);
        }
        if (ring->sqes_ptr && ring->sqes_ptr != MAP_FAILED) {
            munmap(ring->sqes_ptr, ring->sqes_size);
        }
    }
#endif

    if (ring->ring_fd >= 0) {
        close(ring->ring_fd);
        ring->ring_fd = -1;
    }

    free(ring);
}

int
csilk_wal_ring_is_real_uring(const csilk_wal_ring_t* ring)
{
    return ring ? ring->is_real_uring : 0;
}

int
csilk_wal_ring_submit_writev(
    csilk_wal_ring_t* ring, int fd, const struct iovec* iov, int iovcnt, off_t offset)
{
    if (!ring || fd < 0 || !iov || iovcnt <= 0) {
        return -1;
    }

#if defined(__linux__)
    if (ring->is_real_uring) {
        uint32_t             mask = *ring->sq_kmask;
        uint32_t             idx = ring->sq_tail & mask;
        struct io_uring_sqe* sqe = &ring->sqes[idx];

        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_WRITEV;
        sqe->fd = fd;
        sqe->addr = (uint64_t)(uintptr_t)iov;
        sqe->len = (uint32_t)iovcnt;
        sqe->off = (offset >= 0) ? (uint64_t)offset : 0;
        sqe->user_data = 1;

        ring->sq_array[idx] = idx;
        ring->sq_tail++;
        ring->pending_submits++;
        return 0;
    }
#endif

    /* POSIX fallback emulation */
    ssize_t n;
    if (offset >= 0) {
        n = pwritev(fd, iov, iovcnt, offset);
    } else {
        n = writev(fd, iov, iovcnt);
    }
    if (n < 0) {
        return -1;
    }
    ring->completed_count++;
    return 0;
}

int
csilk_wal_ring_submit_fsync(csilk_wal_ring_t* ring, int fd, int is_fdatasync)
{
    if (!ring || fd < 0) {
        return -1;
    }

#if defined(__linux__)
    if (ring->is_real_uring) {
        uint32_t             mask = *ring->sq_kmask;
        uint32_t             idx = ring->sq_tail & mask;
        struct io_uring_sqe* sqe = &ring->sqes[idx];

        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_FSYNC;
        sqe->fd = fd;
        sqe->fsync_flags = is_fdatasync ? IORING_FSYNC_DATASYNC : 0;
        sqe->user_data = 2;

        ring->sq_array[idx] = idx;
        ring->sq_tail++;
        ring->pending_submits++;
        return 0;
    }
#endif

    /* POSIX fallback emulation */
    int rc;
#if defined(__APPLE__)
    (void)is_fdatasync;
    rc = fsync(fd);
#else
    rc = is_fdatasync ? fdatasync(fd) : fsync(fd);
#endif
    if (rc != 0) {
        return -1;
    }
    ring->completed_count++;
    return 0;
}

int
csilk_wal_ring_submit_writev_fsync(csilk_wal_ring_t*   ring,
                                   int                 fd,
                                   const struct iovec* iov,
                                   int                 iovcnt,
                                   off_t               offset,
                                   int                 is_fdatasync)
{
    if (!ring || fd < 0 || !iov || iovcnt <= 0) {
        return -1;
    }

#if defined(__linux__)
    if (ring->is_real_uring) {
        /* Writev SQE with linked flag */
        uint32_t             mask = *ring->sq_kmask;
        uint32_t             idx1 = ring->sq_tail & mask;
        struct io_uring_sqe* sqe1 = &ring->sqes[idx1];

        memset(sqe1, 0, sizeof(*sqe1));
        sqe1->opcode = IORING_OP_WRITEV;
        sqe1->flags = IOSQE_IO_LINK;
        sqe1->fd = fd;
        sqe1->addr = (uint64_t)(uintptr_t)iov;
        sqe1->len = (uint32_t)iovcnt;
        sqe1->off = (offset >= 0) ? (uint64_t)offset : 0;
        sqe1->user_data = 1;
        ring->sq_array[idx1] = idx1;
        ring->sq_tail++;

        /* Linked Fsync SQE */
        uint32_t             idx2 = ring->sq_tail & mask;
        struct io_uring_sqe* sqe2 = &ring->sqes[idx2];
        memset(sqe2, 0, sizeof(*sqe2));
        sqe2->opcode = IORING_OP_FSYNC;
        sqe2->fd = fd;
        sqe2->fsync_flags = is_fdatasync ? IORING_FSYNC_DATASYNC : 0;
        sqe2->user_data = 2;
        ring->sq_array[idx2] = idx2;
        ring->sq_tail++;

        ring->pending_submits += 2;
        return 0;
    }
#endif

    if (csilk_wal_ring_submit_writev(ring, fd, iov, iovcnt, offset) != 0) {
        return -1;
    }
    return csilk_wal_ring_submit_fsync(ring, fd, is_fdatasync);
}

int
csilk_wal_ring_flush(csilk_wal_ring_t* ring, uint32_t min_complete)
{
    if (!ring) {
        return -1;
    }

#if defined(__linux__)
    if (ring->is_real_uring) {
        *ring->sq_ktail = ring->sq_tail;
        __sync_synchronize();

        uint32_t to_submit = ring->pending_submits;
        unsigned flags = (min_complete > 0) ? IORING_ENTER_GETEVENTS : 0;

        long ret =
            syscall(__NR_io_uring_enter, ring->ring_fd, to_submit, min_complete, flags, NULL, 0);
        if (ret < 0 && errno != EAGAIN && errno != EINTR) {
            return -1;
        }

        ring->pending_submits = 0;

        /* Reap CQEs */
        uint32_t head = *ring->cq_khead;
        uint32_t tail = *ring->cq_ktail;
        uint32_t mask = *ring->cq_kmask;
        int      completed = 0;

        while (head != tail) {
            struct io_uring_cqe* cqe = &ring->cqes[head & mask];
            if (cqe->res >= 0) {
                ring->completed_count++;
                completed++;
            }
            head++;
        }
        *ring->cq_khead = head;
        __sync_synchronize();

        return completed;
    }
#endif

    (void)min_complete;
    return (int)ring->completed_count;
}

uint64_t
csilk_wal_ring_get_completed(const csilk_wal_ring_t* ring)
{
    return ring ? ring->completed_count : 0;
}
