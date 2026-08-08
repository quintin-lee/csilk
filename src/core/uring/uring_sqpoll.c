#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/io_perf.h"

#ifdef CSILK_USE_URING
#include <liburing.h>

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
int
csilk_uring_sqpoll_init(void* ring, int cpu_core, uint32_t idle_ms)
{
    (void)ring;
    (void)cpu_core;
    (void)idle_ms;
    return -1;
}

int
csilk_uring_sqpoll_wakeup(void* ring)
{
    (void)ring;
    return -1;
}
#endif
