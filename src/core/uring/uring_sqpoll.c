/**
 * @file uring_sqpoll.c
 * @brief io_uring SQPOLL (kernel submission polling thread) setup helpers.
 *
 * Initializes an io_uring instance using the SQPOLL feature so the kernel can
 * poll the submission queue without requiring a syscall on every submission,
 * with optional CPU affinity for the poller thread. Falls back to a stub that
 * returns -1 when io_uring is not compiled in.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/uring/io_perf.h"

#ifdef CSILK_USE_URING
#include <liburing.h>

/**
 * @brief Initialize an io_uring ring with the SQPOLL kernel poller.
 * @param[out] ring     io_uring instance to initialize.
 * @param[in]  cpu_core Core to pin the SQPOLL thread to, or -1 for none.
 * @param[in]  idle_ms  Poller idle timeout in milliseconds (defaults to 2000).
 * @return 0 on success, or a negative errno from io_uring_queue_init* on
 *         failure.
 * @note If SQPOLL setup fails (e.g. missing CAP_SYS_ADMIN) the routine falls
 *       back to a standard non-polling ring rather than failing outright.
 */
int
csilk_uring_sqpoll_init(struct io_uring* ring, int cpu_core, uint32_t idle_ms)
{
    if (!ring) {
        return -1;
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = (idle_ms > 0) ? idle_ms : 2000;

    if (cpu_core >= 0) {
        params.flags |= IORING_SETUP_SQ_AFF;
        params.sq_thread_cpu = (uint32_t)cpu_core;
    }

    int res = io_uring_queue_init_params(1024, ring, &params);
    if (res < 0) {
        /* If SQPOLL fails due to permissions (CAP_SYS_ADMIN), fall back to standard ring */
        res = io_uring_queue_init(1024, ring, 0);
    }
    return res;
}

/**
 * @brief Wake the SQPOLL kernel thread if it is currently idle.
 * @param[in] ring io_uring ring created with csilk_uring_sqpoll_init.
 * @return 0 on success or if no wakeup is needed, or a negative errno from
 *         io_uring_enter on failure.
 * @note Only issues an io_uring_enter wakeup when the ring's SQ flags indicate
 *       IORING_SQ_NEED_WAKEUP, i.e. the poller has gone to sleep.
 */
int
csilk_uring_sqpoll_wakeup(struct io_uring* ring)
{
    if (!ring) {
        return -1;
    }
    if (*ring->sq.kflags & IORING_SQ_NEED_WAKEUP) {
        return io_uring_enter(ring->ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, NULL);
    }
    return 0;
}
#else

/**
 * @brief Stub for csilk_uring_sqpoll_init when io_uring is disabled.
 * @param[out] ring   Unused.
 * @param[in]  cpu_core Unused.
 * @param[in]  idle_ms Unused.
 * @return Always -1.
 */
int
csilk_uring_sqpoll_init(void* ring, int cpu_core, uint32_t idle_ms)
{
    (void)ring;
    (void)cpu_core;
    (void)idle_ms;
    return -1;
}

/**
 * @brief Stub for csilk_uring_sqpoll_wakeup when io_uring is disabled.
 * @param[in] ring Unused.
 * @return Always -1.
 */
int
csilk_uring_sqpoll_wakeup(void* ring)
{
    (void)ring;
    return -1;
}
#endif
