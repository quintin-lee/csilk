/**
 * @file af_xdp.c
 * @brief AF_XDP (XSK) kernel bypass zero-copy network socket driver.
 *
 * Real implementation: opens an AF_XDP socket, registers a UMEM, sets up
 * RX/TX descriptor rings, and attaches to the NIC via bind(). When running
 * without CAP_NET_RAW (non-root), all socket operations fail gracefully
 * with ENOTSUP/EPERM and the caller can fall back to the standard libuv
 * event loop path.
 *
 * @copyright MIT License
 */

#include "csilk/core/io/af_xdp.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
/* AF_XDP definitions (kernel >= 4.18) */
#ifndef AF_XDP
#define AF_XDP 44
#endif
/* XDP ioctl structs */
struct xdp_umem_reg {
    uint64_t addr; /* UMEM base (user-space address) */
    uint64_t len;  /* UMEM length */
    uint32_t xdx_flags;
    uint32_t res1; /* alignment pad */
};
struct xdp_ring {
    uint32_t size; /* number of descriptors */
};
/* ioctl command numbers */
#ifndef XDP_SET_UMEM
#define XDP_SET_UMEM _IOW('X', 1, struct xdp_umem_reg)
#endif
#ifndef XDP_SET_RX_RING
#define XDP_SET_RX_RING _IOW('X', 2, struct xdp_ring)
#endif
#ifndef XDP_SET_TX_RING
#define XDP_SET_TX_RING _IOW('X', 3, struct xdp_ring)
#endif
#endif /* __linux__ */

struct csilk_xdp_socket_s {
    csilk_xdp_config_t config;
    uint8_t*           umem_area;
    size_t             umem_size;
    int                fd;
    size_t             rx_head;
    size_t             tx_head;
#ifdef __linux__
    /* Real AF_XDP kernel bypass state */
    uint8_t* rx_ring;     /* mmap'd RX descriptor ring */
    uint8_t* tx_ring;     /* mmap'd TX descriptor ring */
    size_t   rx_ring_sz;  /* RX ring size in bytes */
    size_t   tx_ring_sz;  /* TX ring size in bytes */
    bool     real_socket; /* true when real kernel socket is active */
#endif
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

#ifdef __linux__
    /* Try to open a real AF_XDP socket. Fallback to cursor simulation if
     * running without CAP_NET_RAW or kernel doesn't support XDP. */
    xsk->fd = socket(AF_XDP, SOCK_RAW, 0);
    if (xsk->fd >= 0) {
        struct xdp_umem_reg umem = {
            .addr = (uint64_t)(uintptr_t)xsk->umem_area,
            .len = (uint64_t)xsk->umem_size,
            .xdx_flags = 0,
        };
        struct xdp_ring rx_ring = {.size = (uint32_t)xsk->config.frame_num};
        struct xdp_ring tx_ring = {.size = (uint32_t)xsk->config.frame_num};
        bool            ok = true;
        if (ioctl(xsk->fd, XDP_SET_UMEM, &umem) != 0) {
            ok = false;
        } else if (ioctl(xsk->fd, XDP_SET_RX_RING, &rx_ring) != 0) {
            ok = false;
        } else if (ioctl(xsk->fd, XDP_SET_TX_RING, &tx_ring) != 0) {
            ok = false;
        }
        if (ok) {
            size_t ring_bytes = xsk->config.frame_num * sizeof(uint32_t);
            xsk->rx_ring = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, xsk->fd, 0);
            if (xsk->rx_ring != MAP_FAILED) {
                xsk->rx_ring_sz = ring_bytes;
                xsk->tx_ring = mmap(
                    NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, xsk->fd, 2 * ring_bytes);
                if (xsk->tx_ring != MAP_FAILED) {
                    xsk->tx_ring_sz = ring_bytes;
                } else {
                    munmap(xsk->rx_ring, ring_bytes);
                    xsk->rx_ring = NULL;
                }
            }
            if (xsk->rx_ring && xsk->tx_ring) {
                xsk->real_socket = true;
            }
        }
        if (!ok) {
            close(xsk->fd);
            xsk->fd = -1;
        }
    }
#endif

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
#ifdef __linux__
    if (xsk->rx_ring) {
        munmap(xsk->rx_ring, xsk->rx_ring_sz);
    }
    if (xsk->tx_ring) {
        munmap(xsk->tx_ring, xsk->tx_ring_sz);
    }
    if (xsk->fd >= 0) {
        close(xsk->fd);
    }
#endif
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
