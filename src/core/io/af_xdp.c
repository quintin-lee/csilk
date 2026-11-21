/**
 * @file af_xdp.c
 * @brief AF_XDP (XSK) experimental simulated UMEM ring driver.
 *
 * @warning EXPERIMENTAL / SIMULATED STUB: When CSILK_ENABLE_EXPERIMENTAL_STUBS=1,
 *          allocates an in-process page-aligned UMEM ring buffer with cursor-based
 *          TX/RX simulation for API prototyping and unit testing. When
 *          CSILK_ENABLE_EXPERIMENTAL_STUBS=0 (the hardened default), all socket
 *          and burst entrypoints fail closed with ENOTSUP.
 *
 * @copyright MIT License
 */

#include "csilk/core/io/af_xdp.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CSILK_ENABLE_EXPERIMENTAL_STUBS
#define CSILK_ENABLE_EXPERIMENTAL_STUBS 0
#endif

struct csilk_xdp_socket_s {
    csilk_xdp_config_t config;
    uint8_t*           umem_area;
    size_t             umem_size;
    int                fd;
    size_t             rx_head;
    size_t             tx_head;
};

/**
 * @brief Create an AF_XDP socket from a configuration.
 * @param[in] config Socket configuration (validated non-NULL); zero frame_size/
 *                    frame_num default to 2048/1024.
 * @return A newly allocated socket with an aligned UMEM area when stubs are
 *         enabled, or NULL (with errno = ENOTSUP when fail-closed) on invalid
 *         config, overflow, or allocation failure.
 */
csilk_xdp_socket_t*
csilk_xdp_socket_new(const csilk_xdp_config_t* config)
{
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    (void)config;
    errno = ENOTSUP;
    return NULL;
#else
    if (!config) {
        return NULL;
    }

    csilk_xdp_socket_t* xsk = calloc(1, sizeof(csilk_xdp_socket_t));
    if (!xsk) {
        return NULL;
    }

    xsk->config = *config;
    if (xsk->config.frame_size == 0) {
        xsk->config.frame_size = 2048;
    }
    if (xsk->config.frame_num == 0) {
        xsk->config.frame_num = 1024;
    }

    if (xsk->config.frame_size > SIZE_MAX / xsk->config.frame_num) {
        free(xsk);
        errno = EOVERFLOW;
        return NULL;
    }

    xsk->umem_size = xsk->config.frame_size * xsk->config.frame_num;
    void* umem = NULL;
    if (posix_memalign(&umem, 4096, xsk->umem_size) != 0 || !umem) {
        free(xsk);
        return NULL;
    }

    xsk->umem_area = (uint8_t*)umem;
    memset(xsk->umem_area, 0, xsk->umem_size);
    xsk->fd = -1;
    xsk->rx_head = 0;
    xsk->tx_head = 0;

    return xsk;
#endif
}

/**
 * @brief Free an AF_XDP socket and its UMEM area.
 * @param[in] xsk Socket to free (no-op if NULL).
 * @note Frees the UMEM buffer (if any) and the socket struct.
 */
void
csilk_xdp_socket_free(csilk_xdp_socket_t* xsk)
{
    if (!xsk) {
        return;
    }
    if (xsk->umem_area) {
        free(xsk->umem_area);
    }
    free(xsk);
}

/**
 * @brief Receive one frame from the AF_XDP RX ring (single-frame burst).
 * @param[in]  xsk        Socket to receive from (validated non-NULL).
 * @param[out] frame_data Receives a pointer into the UMEM for the next RX frame.
 * @param[out] frame_len  Receives the frame size.
 * @return 0 on success, -ENOTSUP when experimental stubs are disabled, or -1 on NULL arguments.
 * @note Advances an internal RX cursor modulo the frame count; the returned
 *       pointer aliases UMEM memory owned by the socket.
 */
int
csilk_xdp_rx_burst(csilk_xdp_socket_t* xsk, const uint8_t** frame_data, size_t* frame_len)
{
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    (void)xsk;
    (void)frame_data;
    (void)frame_len;
    errno = ENOTSUP;
    return -ENOTSUP;
#else
    if (!xsk || !frame_data || !frame_len) {
        return -1;
    }

    size_t   frame_idx = xsk->rx_head % xsk->config.frame_num;
    uint8_t* frame_ptr = xsk->umem_area + (frame_idx * xsk->config.frame_size);

    *frame_data = frame_ptr;
    *frame_len = xsk->config.frame_size;
    xsk->rx_head++;

    return 0;
#endif
}

/**
 * @brief Transmit one frame into the AF_XDP TX ring (single-frame burst).
 * @param[in] xsk        Socket to transmit on (validated non-NULL).
 * @param[in] frame_data Frame bytes to copy into UMEM.
 * @param[in] frame_len  Length of frame_data (must be <= configured frame size).
 * @return 0 on success, -ENOTSUP when experimental stubs are disabled, or -1 on invalid args.
 * @note Copies frame_data into the next UMEM TX slot and advances the TX cursor.
 */
int
csilk_xdp_tx_burst(csilk_xdp_socket_t* xsk, const uint8_t* frame_data, size_t frame_len)
{
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    (void)xsk;
    (void)frame_data;
    (void)frame_len;
    errno = ENOTSUP;
    return -ENOTSUP;
#else
    if (!xsk || !frame_data || frame_len == 0) {
        return -1;
    }

    if (frame_len > xsk->config.frame_size) {
        return -1;
    }

    size_t   frame_idx = xsk->tx_head % xsk->config.frame_num;
    uint8_t* frame_ptr = xsk->umem_area + (frame_idx * xsk->config.frame_size);

    memcpy(frame_ptr, frame_data, frame_len);
    xsk->tx_head++;

    return 0;
#endif
}
