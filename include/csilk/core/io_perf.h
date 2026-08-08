/**
 * @file io_perf.h
 * @brief Ultra-high performance I/O engine interfaces (AF_XDP Zero-Copy & io_uring SQPOLL).
 *
 * Provides capabilities for probing Linux kernel and NIC drivers, enabling AF_XDP
 * hardware zero-copy UMEM packet processing, and configuring zero-syscall io_uring
 * SQPOLL kernel thread polling.
 */

#ifndef CSILK_IO_PERF_H
#define CSILK_IO_PERF_H

#include <stddef.h>
#include <stdint.h>
#include "csilk/app/app.h"
#include "csilk/csilk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CSILK_IO_MODE_STANDARD = 0, /**< Standard libuv / socket event loop */
    CSILK_IO_MODE_URING = 1,    /**< Basic io_uring polling */
    CSILK_IO_MODE_SQPOLL = 2,   /**< io_uring kernel SQPOLL zero-syscall mode */
    CSILK_IO_MODE_XDP = 3       /**< AF_XDP hardware zero-copy UMEM bypass */
} csilk_io_mode_t;

typedef struct {
    csilk_io_mode_t active_mode;      /**< Currently active I/O mode */
    int             has_xdp_zerocopy; /**< True if NIC driver supports zero-copy */
    int             has_sqpoll;       /**< True if kernel SQPOLL is enabled */
    int             has_iopoll;       /**< True if IOPOLL is supported */
    char            nic_name[32];     /**< Bound physical NIC interface name */
} csilk_io_perf_info_t;

/**
 * @brief Probes Linux kernel capabilities and hardware NIC features.
 * @return Probe result structure.
 */
csilk_io_perf_info_t csilk_io_perf_probe(void);

/**
 * @brief Enables AF_XDP zero-copy hardware bypass engine.
 * @param app Application handle.
 * @param ifname Physical NIC interface name (e.g., "eth0").
 * @param queue_id NIC hardware queue ID.
 * @return 0 on success, negative value on failure and fallback.
 */
int csilk_io_perf_enable_xdp(csilk_app_t* app, const char* ifname, uint32_t queue_id);

/**
 * @brief Enables io_uring kernel SQPOLL zero-syscall polling.
 * @param server Server handle.
 * @param cpu_core Target CPU core ID for SQ thread pinning.
 * @return 0 on success, negative value on failure and fallback.
 */
int csilk_io_perf_enable_sqpoll(csilk_server_t* server, int cpu_core);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_IO_PERF_H */
