#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/core/io_perf.h"

int csilk_uring_sqpoll_init(void* ring, int cpu_core, uint32_t idle_ms);
int csilk_uring_sqpoll_wakeup(void* ring);

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
    printf("All test_uring_sqpoll tests passed!\n");
    return 0;
}
