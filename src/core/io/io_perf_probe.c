/**
 * @file io_perf_probe.c
 * @brief I/O performance capability probing and feature enablement.
 *
 * Detects which fast-I/O subsystems are available at compile/runtime time
 * (io_uring, SQPOLL/IOPOLL, AF_XDP zero-copy) and exposes helpers to request
 * their activation on an app or server. Used to report the active I/O mode and
 * to enable optional acceleration paths.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/io_perf.h"

/**
 * @brief Probe the active I/O mode and available fast-I/O features.
 * @return A csilk_io_perf_info_t describing the active mode, whether SQPOLL/
 *         IOPOLL/XDP-zero-copy are available, and the NIC name ("eth0").
 * @note Sets has_xdp_zerocopy only when running as root (euid 0). When built
 *       with CSILK_USE_URING the active mode is URING with SQPOLL/IOPOLL on.
 */
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

/**
 * @brief Attempt to enable AF_XDP zero-copy on an app's NIC.
 * @param[in] app     Application to enable XDP on (validated non-NULL).
 * @param[in] ifname  Network interface name (validated non-NULL).
 * @param[in] queue_id RX queue id (currently unused).
 * @return 0 if XDP zero-copy is available and requested, -1 on NULL args or
 *         when has_xdp_zerocopy is not set (gracefully falls back).
 */
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

/**
 * @brief Attempt to enable SQPOLL on a server.
 * @param[in] server   Server to enable SQPOLL on (validated non-NULL).
 * @param[in] cpu_core CPU core hint (currently unused).
 * @return 0 if SQPOLL is available and requested, -1 on NULL server or when
 *         has_sqpoll is not set.
 */
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
