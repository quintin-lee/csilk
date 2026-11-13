#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/core/uring/io_perf.h"

#ifdef CSILK_USE_URING
#include <liburing.h>

int csilk_uring_sqpoll_init(struct io_uring* ring, int cpu_core, uint32_t idle_ms);
int csilk_uring_sqpoll_wakeup(struct io_uring* ring);

static void
test_sqpoll_real_ring(void)
{
    struct io_uring ring;
    int             res = csilk_uring_sqpoll_init(&ring, -1, 0);
    if (res < 0) {
        printf("test_sqpoll_real_ring skipped (sqpoll_init=%d)\n", res);
        return;
    }

    int wake_res = csilk_uring_sqpoll_wakeup(&ring);
    assert(wake_res >= 0);
    io_uring_queue_exit(&ring);

    struct io_uring aff_ring;
    res = csilk_uring_sqpoll_init(&aff_ring, 0, 50);
    if (res == 0) {
        wake_res = csilk_uring_sqpoll_wakeup(&aff_ring);
        assert(wake_res >= 0);
        io_uring_queue_exit(&aff_ring);
    }

    printf("test_sqpoll_real_ring passed\n");
}
#else
int csilk_uring_sqpoll_init(void* ring, int cpu_core, uint32_t idle_ms);
int csilk_uring_sqpoll_wakeup(void* ring);

static void
test_sqpoll_stub(void)
{
    int dummy = 0;
    assert(csilk_uring_sqpoll_init(&dummy, 0, 100) == -1);
    assert(csilk_uring_sqpoll_wakeup(&dummy) == -1);
    printf("test_sqpoll_stub passed\n");
}
#endif

static void
test_sqpoll_dummy_fallback(void)
{
    int res = csilk_uring_sqpoll_init(NULL, 0, 1000);
    assert(res == -1);

    res = csilk_uring_sqpoll_wakeup(NULL);
    assert(res == -1);

    printf("test_sqpoll_dummy_fallback passed\n");
}

int
main(void)
{
    test_sqpoll_dummy_fallback();
#ifdef CSILK_USE_URING
    test_sqpoll_real_ring();
#else
    test_sqpoll_stub();
#endif
    printf("All test_uring_sqpoll tests passed!\n");
    return 0;
}
