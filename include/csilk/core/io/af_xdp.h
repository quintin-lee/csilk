#pragma once
/**
 * @file af_xdp.h
 * @brief AF_XDP (XSK) kernel bypass zero-copy network socket driver.
 *
 * Implements real Linux AF_XDP (XSK) kernel bypass zero-copy network driver
 * utilizing 4-ring descriptor queues (UMEM Fill Ring, Completion Ring, RX Ring,
 * and TX Ring). When running without CAP_NET_RAW / root or on non-Linux systems,
 * seamlessly falls back to high-performance in-memory ring simulation.
 *
 * @version 1.2.0
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_xdp_socket_s csilk_xdp_socket_t;

/**
 * @brief Performance and packet statistics for AF_XDP socket.
 */
typedef struct {
    uint64_t rx_packets; /**< Total received packets */
    uint64_t rx_bytes;   /**< Total received bytes */
    uint64_t rx_dropped; /**< Dropped RX packets (e.g. ring full) */
    uint64_t tx_packets; /**< Total transmitted packets */
    uint64_t tx_bytes;   /**< Total transmitted bytes */
    uint64_t tx_dropped; /**< Dropped TX packets */
} csilk_xdp_stats_t;

/**
 * @brief Configuration parameters for AF_XDP socket initialization.
 */
typedef struct {
    const char* ifname;     /**< Network interface name (e.g. "eth0" or "lo"). */
    uint32_t    queue_id;   /**< RX/TX queue ID on the network interface. */
    size_t      frame_size; /**< Size of each UMEM frame (typically 2048 or 4096). */
    size_t      frame_num;  /**< Total number of frames in UMEM pool. */
    uint32_t    xdp_flags;  /**< XDP bind flags (SKB, DRV, ZEROCOPY, COPY). */
    uint32_t    bind_flags; /**< XSK socket bind flags. */
} csilk_xdp_config_t;

/**
 * @brief Create and initialize a new AF_XDP socket and UMEM ring buffers.
 * @param config Socket configuration options.
 * @return Pointer to initialized socket, or NULL on failure.
 */
csilk_xdp_socket_t* csilk_xdp_socket_new(const csilk_xdp_config_t* config);

/**
 * @brief Destroy an AF_XDP socket and release UMEM frame resources.
 * @param xsk AF_XDP socket instance.
 */
void csilk_xdp_socket_free(csilk_xdp_socket_t* xsk);

/**
 * @brief Non-blocking receive raw ethernet frames from AF_XDP RX ring.
 * @param xsk AF_XDP socket instance.
 * @param[out] frame_data Pointer to frame payload buffer.
 * @param[out] frame_len Receives frame size in bytes.
 * @return 0 on success (frame received), -1 if no frames available.
 */
int csilk_xdp_rx_burst(csilk_xdp_socket_t* xsk, const uint8_t** frame_data, size_t* frame_len);

/**
 * @brief Non-blocking receive batch of raw ethernet frames from AF_XDP RX ring.
 * @param xsk AF_XDP socket instance.
 * @param[out] frame_data_arr Array to receive frame payload pointers.
 * @param[out] frame_len_arr Array to receive frame lengths.
 * @param max_burst Maximum frames to receive.
 * @return Number of frames received (>= 0), or -1 on invalid arguments.
 */
int csilk_xdp_rx_burst_batch(csilk_xdp_socket_t* xsk,
                             const uint8_t**     frame_data_arr,
                             size_t*             frame_len_arr,
                             size_t              max_burst);

/**
 * @brief Non-blocking transmit raw ethernet frames to AF_XDP TX ring.
 * @param xsk AF_XDP socket instance.
 * @param frame_data Frame payload buffer to send.
 * @param frame_len Frame size in bytes.
 * @return 0 on success, -1 on failure.
 */
int csilk_xdp_tx_burst(csilk_xdp_socket_t* xsk, const uint8_t* frame_data, size_t frame_len);

/**
 * @brief Produce a free UMEM frame address into the Fill Ring for kernel/NIC RX.
 * @param xsk AF_XDP socket instance.
 * @param addr Offset within UMEM to submit.
 * @return 0 on success, -1 if ring is full or invalid args.
 */
int csilk_xdp_fill_ring_produce(csilk_xdp_socket_t* xsk, uint64_t addr);

/**
 * @brief Reap a completed transmission frame address from the Completion Ring.
 * @param xsk AF_XDP socket instance.
 * @param[out] addr Receives completed UMEM frame offset.
 * @return 0 on success (reaped), -1 if ring is empty.
 */
int csilk_xdp_complete_ring_reap(csilk_xdp_socket_t* xsk, uint64_t* addr);

/**
 * @brief Check if socket is bound to a real kernel AF_XDP socket.
 * @param xsk AF_XDP socket instance.
 * @return true if running on real Linux AF_XDP socket, false if running in simulation.
 */
bool csilk_xdp_is_real_socket(const csilk_xdp_socket_t* xsk);

/**
 * @brief Retrieve current packet and byte statistics.
 * @param xsk AF_XDP socket instance.
 * @param[out] stats Pointer to stats structure.
 * @return 0 on success, -1 on error.
 */
int csilk_xdp_get_stats(const csilk_xdp_socket_t* xsk, csilk_xdp_stats_t* stats);

/**
 * @brief Get the underlying file descriptor for poll / epoll integration.
 * @param xsk AF_XDP socket instance.
 * @return Socket file descriptor, or -1 if not applicable.
 */
int csilk_xdp_get_fd(const csilk_xdp_socket_t* xsk);

#ifdef __cplusplus
}
#endif
