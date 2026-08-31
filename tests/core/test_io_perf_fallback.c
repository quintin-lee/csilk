#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/core/uring/io_perf.h"

static void
test_io_perf_probing_and_fallback(void)
{
    csilk_io_perf_info_t info = csilk_io_perf_probe();
    assert(info.nic_name[0] != '\0');

    int xdp_res = csilk_io_perf_enable_xdp(NULL, "eth0", 0);
    assert(xdp_res == -1);

    int sqpoll_res = csilk_io_perf_enable_sqpoll(NULL, 0);
    assert(sqpoll_res == -1);

    printf("test_io_perf_probing_and_fallback passed\n");
}

int
main(void)
{
    test_io_perf_probing_and_fallback();
    printf("All test_io_perf_fallback tests passed!\n");
    return 0;
}
