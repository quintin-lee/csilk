/**
 * @file af_xdp.c
 * @brief AF_XDP (XSK) kernel bypass zero-copy network socket driver.
 *
 * Real Linux AF_XDP (XSK) kernel bypass driver with 4-ring DMA descriptor
 * queues (Fill, Completion, RX, TX) and zero-copy UMEM support. When running
 * without CAP_NET_RAW / root or on non-Linux platforms, gracefully falls back
 * to high-performance in-memory ring simulation.
 *
 * @copyright MIT License
 */

#include "csilk/core/io/af_xdp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AF_XDP
#define AF_XDP 44
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif
#endif /* __linux__ */

/* --- Internal Type Definitions --- */

#if defined(__linux__)
typedef struct {
    uint32_t* producer;
    uint32_t* consumer;
    void*     desc;
    uint32_t* flags;
    uint32_t  size;
    uint32_t  mask;
    uint32_t  cached_prod;
    uint32_t  cached_cons;
} xdp_ring_t;
#endif

struct csilk_xdp_socket_s {
    csilk_xdp_config_t config;
    uint8_t*           umem_area;
    size_t             umem_size;
    int                fd;
    size_t             rx_head;
    size_t             tx_head;
    bool               real_socket;
    csilk_xdp_stats_t  stats;

#if defined(__linux__)
    xdp_ring_t rx;
    xdp_ring_t tx;
    xdp_ring_t fr; /* Fill Ring */
    xdp_ring_t cr; /* Completion Ring */

    void*  rx_map;
    size_t rx_map_sz;
    void*  tx_map;
    size_t tx_map_sz;
    void*  fr_map;
    size_t fr_map_sz;
    void*  cr_map;
    size_t cr_map_sz;
#endif

    /* Simulation queues (used when real_socket == false) */
    uint64_t* sim_fr_addrs;
    size_t    sim_fr_head;
    size_t    sim_fr_tail;
    size_t    sim_fr_count;

    uint64_t* sim_cr_addrs;
    size_t    sim_cr_head;
    size_t    sim_cr_tail;
    size_t    sim_cr_count;
};

/* --- Public Function Implementations --- */

/**
 * @brief Create an AF_XDP socket from a configuration.
 * @param[in] config Socket configuration (validated non-NULL); zero frame_size/
 *                    frame_num default to 2048/1024.
 * @return A newly allocated socket with an aligned UMEM area, or NULL on invalid
 *         config or allocation failure.
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
    xsk->real_socket = false;

#if defined(__linux__)
    /* Attempt to initialize real AF_XDP Linux socket if privileged */
    xsk->fd = socket(AF_XDP, SOCK_RAW, 0);
    if (xsk->fd >= 0) {
        struct xdp_umem_reg mr = {
            .addr = (uintptr_t)xsk->umem_area,
            .len = (uint64_t)xsk->umem_size,
            .chunk_size = (uint32_t)xsk->config.frame_size,
            .headroom = 0,
            .flags = 0,
        };
        uint32_t ring_size = (uint32_t)xsk->config.frame_num;

        bool ok = true;
        if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr)) != 0) {
            ok = false;
        } else if (setsockopt(
                       xsk->fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring_size, sizeof(ring_size)) != 0) {
            ok = false;
        } else if (setsockopt(
                       xsk->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_size, sizeof(ring_size)) !=
                   0) {
            ok = false;
        } else if (setsockopt(xsk->fd, SOL_XDP, XDP_RX_RING, &ring_size, sizeof(ring_size)) != 0) {
            ok = false;
        } else if (setsockopt(xsk->fd, SOL_XDP, XDP_TX_RING, &ring_size, sizeof(ring_size)) != 0) {
            ok = false;
        }

        if (ok) {
            struct xdp_mmap_offsets off = {0};
            socklen_t               optlen = sizeof(off);
            if (getsockopt(xsk->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) == 0) {
                /* 1. Map Fill Ring */
                size_t fr_sz = off.fr.desc + ring_size * sizeof(uint64_t);
                xsk->fr_map = mmap(NULL,
                                   fr_sz,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE,
                                   xsk->fd,
                                   XDP_UMEM_PGOFF_FILL_RING);
                if (xsk->fr_map != MAP_FAILED) {
                    xsk->fr_map_sz = fr_sz;
                    xsk->fr.producer = (uint32_t*)((uint8_t*)xsk->fr_map + off.fr.producer);
                    xsk->fr.consumer = (uint32_t*)((uint8_t*)xsk->fr_map + off.fr.consumer);
                    xsk->fr.desc = (void*)((uint8_t*)xsk->fr_map + off.fr.desc);
                    xsk->fr.flags = (uint32_t*)((uint8_t*)xsk->fr_map + off.fr.flags);
                    xsk->fr.size = ring_size;
                    xsk->fr.mask = ring_size - 1;
                } else {
                    ok = false;
                }

                /* 2. Map Completion Ring */
                if (ok) {
                    size_t cr_sz = off.cr.desc + ring_size * sizeof(uint64_t);
                    xsk->cr_map = mmap(NULL,
                                       cr_sz,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_POPULATE,
                                       xsk->fd,
                                       XDP_UMEM_PGOFF_COMPLETION_RING);
                    if (xsk->cr_map != MAP_FAILED) {
                        xsk->cr_map_sz = cr_sz;
                        xsk->cr.producer = (uint32_t*)((uint8_t*)xsk->cr_map + off.cr.producer);
                        xsk->cr.consumer = (uint32_t*)((uint8_t*)xsk->cr_map + off.cr.consumer);
                        xsk->cr.desc = (void*)((uint8_t*)xsk->cr_map + off.cr.desc);
                        xsk->cr.flags = (uint32_t*)((uint8_t*)xsk->cr_map + off.cr.flags);
                        xsk->cr.size = ring_size;
                        xsk->cr.mask = ring_size - 1;
                    } else {
                        ok = false;
                    }
                }

                /* 3. Map RX Ring */
                if (ok) {
                    size_t rx_sz = off.rx.desc + ring_size * sizeof(struct xdp_desc);
                    xsk->rx_map = mmap(NULL,
                                       rx_sz,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_POPULATE,
                                       xsk->fd,
                                       XDP_PGOFF_RX_RING);
                    if (xsk->rx_map != MAP_FAILED) {
                        xsk->rx_map_sz = rx_sz;
                        xsk->rx.producer = (uint32_t*)((uint8_t*)xsk->rx_map + off.rx.producer);
                        xsk->rx.consumer = (uint32_t*)((uint8_t*)xsk->rx_map + off.rx.consumer);
                        xsk->rx.desc = (void*)((uint8_t*)xsk->rx_map + off.rx.desc);
                        xsk->rx.flags = (uint32_t*)((uint8_t*)xsk->rx_map + off.rx.flags);
                        xsk->rx.size = ring_size;
                        xsk->rx.mask = ring_size - 1;
                    } else {
                        ok = false;
                    }
                }

                /* 4. Map TX Ring */
                if (ok) {
                    size_t tx_sz = off.tx.desc + ring_size * sizeof(struct xdp_desc);
                    xsk->tx_map = mmap(NULL,
                                       tx_sz,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_POPULATE,
                                       xsk->fd,
                                       XDP_PGOFF_TX_RING);
                    if (xsk->tx_map != MAP_FAILED) {
                        xsk->tx_map_sz = tx_sz;
                        xsk->tx.producer = (uint32_t*)((uint8_t*)xsk->tx_map + off.tx.producer);
                        xsk->tx.consumer = (uint32_t*)((uint8_t*)xsk->tx_map + off.tx.consumer);
                        xsk->tx.desc = (void*)((uint8_t*)xsk->tx_map + off.tx.desc);
                        xsk->tx.flags = (uint32_t*)((uint8_t*)xsk->tx_map + off.tx.flags);
                        xsk->tx.size = ring_size;
                        xsk->tx.mask = ring_size - 1;
                    } else {
                        ok = false;
                    }
                }

                /* 5. Bind socket to network interface */
                if (ok) {
                    const char*  ifname = xsk->config.ifname ? xsk->config.ifname : "lo";
                    unsigned int ifidx = if_nametoindex(ifname);
                    if (ifidx > 0) {
                        struct sockaddr_xdp sxdp = {0};
                        sxdp.sxdp_family = AF_XDP;
                        sxdp.sxdp_ifindex = ifidx;
                        sxdp.sxdp_queue_id = xsk->config.queue_id;
                        sxdp.sxdp_flags = (uint16_t)xsk->config.bind_flags;
                        if (bind(xsk->fd, (struct sockaddr*)&sxdp, sizeof(sxdp)) == 0) {
                            xsk->real_socket = true;
                        } else {
                            ok = false;
                        }
                    } else {
                        ok = false;
                    }
                }
            } else {
                ok = false;
            }
        }

        if (!ok) {
            /* If real socket setup failed, cleanup and use simulation */
            if (xsk->rx_map && xsk->rx_map != MAP_FAILED) {
                munmap(xsk->rx_map, xsk->rx_map_sz);
            }
            if (xsk->tx_map && xsk->tx_map != MAP_FAILED) {
                munmap(xsk->tx_map, xsk->tx_map_sz);
            }
            if (xsk->fr_map && xsk->fr_map != MAP_FAILED) {
                munmap(xsk->fr_map, xsk->fr_map_sz);
            }
            if (xsk->cr_map && xsk->cr_map != MAP_FAILED) {
                munmap(xsk->cr_map, xsk->cr_map_sz);
            }
            xsk->rx_map = xsk->tx_map = xsk->fr_map = xsk->cr_map = NULL;
            close(xsk->fd);
            xsk->fd = -1;
            xsk->real_socket = false;
        }
    }
#endif

    /* Setup simulation buffers */
    xsk->sim_fr_addrs = calloc(xsk->config.frame_num, sizeof(uint64_t));
    xsk->sim_cr_addrs = calloc(xsk->config.frame_num, sizeof(uint64_t));
    xsk->sim_fr_head = 0;
    xsk->sim_fr_tail = 0;
    xsk->sim_fr_count = 0;
    xsk->sim_cr_head = 0;
    xsk->sim_cr_tail = 0;
    xsk->sim_cr_count = 0;

    return xsk;
}

/**
 * @brief Free an AF_XDP socket and its UMEM area.
 * @param[in] xsk Socket to free (no-op if NULL).
 */
void
csilk_xdp_socket_free(csilk_xdp_socket_t* xsk)
{
    if (!xsk) {
        return;
    }
#if defined(__linux__)
    if (xsk->rx_map && xsk->rx_map != MAP_FAILED) {
        munmap(xsk->rx_map, xsk->rx_map_sz);
    }
    if (xsk->tx_map && xsk->tx_map != MAP_FAILED) {
        munmap(xsk->tx_map, xsk->tx_map_sz);
    }
    if (xsk->fr_map && xsk->fr_map != MAP_FAILED) {
        munmap(xsk->fr_map, xsk->fr_map_sz);
    }
    if (xsk->cr_map && xsk->cr_map != MAP_FAILED) {
        munmap(xsk->cr_map, xsk->cr_map_sz);
    }
    if (xsk->fd >= 0) {
        close(xsk->fd);
    }
#endif
    if (xsk->sim_fr_addrs) {
        free(xsk->sim_fr_addrs);
    }
    if (xsk->sim_cr_addrs) {
        free(xsk->sim_cr_addrs);
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
 * @return 0 on success, -1 on NULL arguments or when no frames are available.
 */
int
csilk_xdp_rx_burst(csilk_xdp_socket_t* xsk, const uint8_t** frame_data, size_t* frame_len)
{
    if (!xsk || !frame_data || !frame_len) {
        return -1;
    }

#if defined(__linux__)
    if (xsk->real_socket) {
        uint32_t cons = *xsk->rx.consumer;
        uint32_t prod = *xsk->rx.producer;
        if (prod != cons) {
            struct xdp_desc* d = &((struct xdp_desc*)xsk->rx.desc)[cons & xsk->rx.mask];
            *frame_data = xsk->umem_area + d->addr;
            *frame_len = d->len;
            (*xsk->rx.consumer)++;
            xsk->stats.rx_packets++;
            xsk->stats.rx_bytes += d->len;
            return 0;
        }
        return -1;
    }
#endif

    size_t   frame_idx = xsk->rx_head % xsk->config.frame_num;
    uint8_t* frame_ptr = xsk->umem_area + (frame_idx * xsk->config.frame_size);

    *frame_data = frame_ptr;
    *frame_len = xsk->config.frame_size;
    xsk->rx_head++;
    xsk->stats.rx_packets++;
    xsk->stats.rx_bytes += xsk->config.frame_size;

    return 0;
}

/**
 * @brief Non-blocking receive batch of raw ethernet frames from AF_XDP RX ring.
 */
int
csilk_xdp_rx_burst_batch(csilk_xdp_socket_t* xsk,
                         const uint8_t**     frame_data_arr,
                         size_t*             frame_len_arr,
                         size_t              max_burst)
{
    if (!xsk || !frame_data_arr || !frame_len_arr || max_burst == 0) {
        return -1;
    }

    size_t count = 0;
    while (count < max_burst) {
        const uint8_t* data = NULL;
        size_t         len = 0;
        if (csilk_xdp_rx_burst(xsk, &data, &len) != 0) {
            break;
        }
        frame_data_arr[count] = data;
        frame_len_arr[count] = len;
        count++;
    }
    return (int)count;
}

/**
 * @brief Transmit one frame into the AF_XDP TX ring (single-frame burst).
 * @param[in] xsk        Socket to transmit on (validated non-NULL).
 * @param[in] frame_data Frame bytes to copy into UMEM.
 * @param[in] frame_len  Length of frame_data (must be <= configured frame size).
 * @return 0 on success, -1 on NULL args, zero length, or oversized frame.
 */
int
csilk_xdp_tx_burst(csilk_xdp_socket_t* xsk, const uint8_t* frame_data, size_t frame_len)
{
    if (!xsk || !frame_data || frame_len == 0 || frame_len > xsk->config.frame_size) {
        return -1;
    }

#if defined(__linux__)
    if (xsk->real_socket) {
        uint32_t prod = *xsk->tx.producer;
        uint32_t cons = *xsk->tx.consumer;
        if (prod - cons >= xsk->tx.size) {
            xsk->stats.tx_dropped++;
            return -1;
        }
        size_t   frame_idx = prod & xsk->tx.mask;
        uint64_t addr = frame_idx * xsk->config.frame_size;
        uint8_t* slot = xsk->umem_area + addr;
        memcpy(slot, frame_data, frame_len);

        struct xdp_desc* d = &((struct xdp_desc*)xsk->tx.desc)[frame_idx];
        d->addr = addr;
        d->len = (uint32_t)frame_len;
        d->options = 0;

        (*xsk->tx.producer)++;
        xsk->stats.tx_packets++;
        xsk->stats.tx_bytes += frame_len;

        /* Kick NIC DMA transmit */
        sendto(xsk->fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
        return 0;
    }
#endif

    size_t   frame_idx = xsk->tx_head % xsk->config.frame_num;
    uint8_t* frame_ptr = xsk->umem_area + (frame_idx * xsk->config.frame_size);

    memcpy(frame_ptr, frame_data, frame_len);
    xsk->tx_head++;
    xsk->stats.tx_packets++;
    xsk->stats.tx_bytes += frame_len;

    if (xsk->sim_cr_addrs && xsk->sim_cr_count < xsk->config.frame_num) {
        xsk->sim_cr_addrs[xsk->sim_cr_tail % xsk->config.frame_num] =
            frame_idx * xsk->config.frame_size;
        xsk->sim_cr_tail++;
        xsk->sim_cr_count++;
    }

    return 0;
}

/**
 * @brief Produce a free UMEM frame address into the Fill Ring.
 */
int
csilk_xdp_fill_ring_produce(csilk_xdp_socket_t* xsk, uint64_t addr)
{
    if (!xsk) {
        return -1;
    }

#if defined(__linux__)
    if (xsk->real_socket) {
        uint32_t prod = *xsk->fr.producer;
        uint32_t cons = *xsk->fr.consumer;
        if (prod - cons >= xsk->fr.size) {
            return -1;
        }
        ((uint64_t*)xsk->fr.desc)[prod & xsk->fr.mask] = addr;
        (*xsk->fr.producer)++;
        return 0;
    }
#endif

    if (!xsk->sim_fr_addrs || xsk->sim_fr_count >= xsk->config.frame_num) {
        return -1;
    }
    xsk->sim_fr_addrs[xsk->sim_fr_tail % xsk->config.frame_num] = addr;
    xsk->sim_fr_tail++;
    xsk->sim_fr_count++;
    return 0;
}

/**
 * @brief Reap a completed transmission frame address from the Completion Ring.
 */
int
csilk_xdp_complete_ring_reap(csilk_xdp_socket_t* xsk, uint64_t* addr)
{
    if (!xsk || !addr) {
        return -1;
    }

#if defined(__linux__)
    if (xsk->real_socket) {
        uint32_t cons = *xsk->cr.consumer;
        uint32_t prod = *xsk->cr.producer;
        if (prod == cons) {
            return -1;
        }
        *addr = ((uint64_t*)xsk->cr.desc)[cons & xsk->cr.mask];
        (*xsk->cr.consumer)++;
        return 0;
    }
#endif

    if (!xsk->sim_cr_addrs || xsk->sim_cr_count == 0) {
        return -1;
    }
    *addr = xsk->sim_cr_addrs[xsk->sim_cr_head % xsk->config.frame_num];
    xsk->sim_cr_head++;
    xsk->sim_cr_count--;
    return 0;
}

/**
 * @brief Check if socket is bound to a real kernel AF_XDP socket.
 */
bool
csilk_xdp_is_real_socket(const csilk_xdp_socket_t* xsk)
{
    return xsk ? xsk->real_socket : false;
}

/**
 * @brief Retrieve current packet and byte statistics.
 */
int
csilk_xdp_get_stats(const csilk_xdp_socket_t* xsk, csilk_xdp_stats_t* stats)
{
    if (!xsk || !stats) {
        return -1;
    }
    *stats = xsk->stats;

#if defined(__linux__)
    if (xsk->real_socket && xsk->fd >= 0) {
        struct xdp_statistics xdp_stat = {0};
        socklen_t             optlen = sizeof(xdp_stat);
        if (getsockopt(xsk->fd, SOL_XDP, XDP_STATISTICS, &xdp_stat, &optlen) == 0) {
            stats->rx_dropped += xdp_stat.rx_dropped;
        }
    }
#endif

    return 0;
}

/**
 * @brief Get the underlying file descriptor for poll / epoll integration.
 */
int
csilk_xdp_get_fd(const csilk_xdp_socket_t* xsk)
{
    return xsk ? xsk->fd : -1;
}
