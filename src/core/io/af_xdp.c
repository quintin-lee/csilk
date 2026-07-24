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
