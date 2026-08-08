#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/io_perf.h"

csilk_io_perf_info_t
csilk_io_perf_probe(void)
{
    csilk_io_perf_info_t info;
    memset(&info, 0, sizeof(info));

    info.active_mode = CSILK_IO_MODE_STANDARD;

#ifdef CSILK_USE_URING
    info.active_mode = CSILK_IO_MODE_URING;
    info.has_sqpoll = 1;
    info.has_iopoll = 1;
#endif

    /* Check CAP_SYS_ADMIN / CAP_NET_RAW root privilege for AF_XDP zero-copy */
    if (geteuid() == 0) {
        info.has_xdp_zerocopy = 1;
    }

    snprintf(info.nic_name, sizeof(info.nic_name), "eth0");
    return info;
}

int
csilk_io_perf_enable_xdp(csilk_app_t* app, const char* ifname, uint32_t queue_id)
{
    if (!app || !ifname) {
        return -1;
    }

    csilk_io_perf_info_t info = csilk_io_perf_probe();
    if (!info.has_xdp_zerocopy) {
        /* Gracefully fall back to SQPOLL or standard mode */
        return -1;
    }

    (void)queue_id;
    return 0;
}

int
csilk_io_perf_enable_sqpoll(csilk_server_t* server, int cpu_core)
{
    if (!server) {
        return -1;
    }

    csilk_io_perf_info_t info = csilk_io_perf_probe();
    if (!info.has_sqpoll) {
        return -1;
    }

    (void)cpu_core;
    return 0;
}
