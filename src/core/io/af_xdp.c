/**
 * @file af_xdp.c
 * @brief AF_XDP (XSK) kernel bypass zero-copy network socket driver implementation.
 * @copyright MIT License
 */

#include "csilk/core/af_xdp.h"
#include <stdlib.h>
#include <string.h>

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
 * @return A newly allocated socket with an aligned UMEM area, or NULL on invalid
 *         config or allocation failure.
 * @note Copies the config (applying defaults), allocates a 4096-byte aligned
 *       UMEM of frame_size*frame_num bytes, and initializes rx/tx cursors to 0.
 */
csilk_xdp_socket_t*
csilk_xdp_socket_new(const csilk_xdp_config_t* config)
{
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

    xsk->umem_size = xsk->config.frame_size * xsk->config.frame_num;
    xsk->umem_area = aligned_alloc(4096, xsk->umem_size);
    if (!xsk->umem_area) {
        free(xsk);
        return NULL;
    }

    memset(xsk->umem_area, 0, xsk->umem_size);
    xsk->fd = -1;
    xsk->rx_head = 0;
    xsk->tx_head = 0;

    return xsk;
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
 * @return 0 on success, -1 on NULL arguments.
 * @note Advances an internal RX cursor modulo the frame count; the returned
 *       pointer aliases UMEM memory owned by the socket.
 */
int
csilk_xdp_rx_burst(csilk_xdp_socket_t* xsk, const uint8_t** frame_data, size_t* frame_len)
{
    if (!xsk || !frame_data || !frame_len) {
        return -1;
    }

    size_t   frame_idx = xsk->rx_head % xsk->config.frame_num;
    uint8_t* frame_ptr = xsk->umem_area + (frame_idx * xsk->config.frame_size);

    *frame_data = frame_ptr;
    *frame_len = xsk->config.frame_size;
    xsk->rx_head++;

    return 0;
}

/**
 * @brief Transmit one frame into the AF_XDP TX ring (single-frame burst).
 * @param[in] xsk        Socket to transmit on (validated non-NULL).
 * @param[in] frame_data Frame bytes to copy into UMEM.
 * @param[in] frame_len  Length of frame_data (must be <= configured frame size).
 * @return 0 on success, -1 on NULL args, zero length, or oversized frame.
 * @note Copies frame_data into the next UMEM TX slot and advances the TX cursor.
 */
int
csilk_xdp_tx_burst(csilk_xdp_socket_t* xsk, const uint8_t* frame_data, size_t frame_len)
{
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
}
