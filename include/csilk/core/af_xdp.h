#pragma once
/**
 * @file af_xdp.h
 * @brief AF_XDP (XSK) kernel bypass zero-copy network socket driver.
 *
 * @version 0.4.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_xdp_socket_s csilk_xdp_socket_t;

/**
 * @brief Configuration parameters for AF_XDP socket initialization.
 */
typedef struct {
    const char* ifname;     /**< Network interface name (e.g. "eth0"). */
    uint32_t    queue_id;   /**< RX/TX queue ID on the network interface. */
    size_t      frame_size; /**< Size of each UMEM frame (typically 2048 or 4096). */
    size_t      frame_num;  /**< Total number of frames in UMEM pool. */
} csilk_xdp_config_t;

/**
 * @brief Create and initialize a new AF_XDP socket and UMEM ring buffers.
 * @param config Socket configuration options.
 * @return Pointer to initialized socket, or nullptr on failure.
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
 * @brief Non-blocking transmit raw ethernet frames to AF_XDP TX ring.
 * @param xsk AF_XDP socket instance.
 * @param frame_data Frame payload buffer to send.
 * @param frame_len Frame size in bytes.
 * @return 0 on success, -1 on failure.
 */
int csilk_xdp_tx_burst(csilk_xdp_socket_t* xsk, const uint8_t* frame_data, size_t frame_len);

#ifdef __cplusplus
}
#endif
