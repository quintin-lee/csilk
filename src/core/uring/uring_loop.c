/**
 * @file uring_loop.c
 * @brief io_uring event loop lifecycle: init, close, stop, now, default_loop.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <string.h>
#include "uring_internal.h"

/* ====================================================================
 * Thread-local default loop state
 * ==================================================================== */

csilk_io_loop_t  g_default_loop;
int              g_default_pending = 0;
int              g_default_loop_inited = 0;
struct io_uring* g_default_ring_ptr = NULL;

/* ====================================================================
 * Loop lifecycle
 * ==================================================================== */

int
csilk_io_loop_init(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    memset(loop, 0, sizeof(*loop));
    return io_uring_queue_init(4096, &loop->ring, 0);
}

int
csilk_io_loop_close(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    io_uring_queue_exit(&loop->ring);
    if (loop == &g_default_loop) {
        g_default_loop_inited = 0;
        g_default_ring_ptr = NULL;
    }
    return 0;
}

void
csilk_io_stop(csilk_io_loop_t* loop)
{
    if (loop) {
        loop->stop_flag = 1;
    }
}

uint64_t
csilk_io_now(const csilk_io_loop_t* loop)
{
    (void)loop;
    return csilk_io_hrtime() / 1000000ULL;
}

void
csilk_io_update_time(csilk_io_loop_t* loop)
{
    (void)loop;
}

csilk_io_loop_t*
csilk_io_default_loop(void)
{
    if (!g_default_loop_inited) {
        g_default_loop_inited = 1;
        if (csilk_io_loop_init(&g_default_loop) != 0) {
            return NULL;
        }
        g_default_ring_ptr = &g_default_loop.ring;
    }
    return &g_default_loop;
}

#endif
