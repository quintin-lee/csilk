#pragma once
/**
 * @file dpdk_pmd.h
 * @brief DPDK (Data Plane Development Kit) Poll Mode Driver user-space network engine.
 *
 * @version 0.5.1
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_dpdk_engine_s csilk_dpdk_engine_t;

/**
 * @brief Configuration parameters for DPDK EAL and PMD port setup.
 */
typedef struct {
    uint16_t port_id;   /**< DPDK port index (e.g. 0). */
    uint16_t rx_rings;  /**< Number of RX queue rings. */
    uint16_t tx_rings;  /**< Number of TX queue rings. */
    uint32_t pool_size; /**< Number of mbuf memory pool buffers. */
    uint32_t mbuf_size; /**< Size of each mbuf packet buffer (e.g. 2048). */
} csilk_dpdk_config_t;

/**
 * @brief Create and initialize a new DPDK Poll Mode Driver engine instance.
 * @param config Engine configuration parameters.
 * @return New engine instance handle, or NULL on failure.
 */
csilk_dpdk_engine_t* csilk_dpdk_engine_new(const csilk_dpdk_config_t* config);

/**
 * @brief Free resources associated with a DPDK engine instance.
 * @param engine Engine instance handle.
 */
void csilk_dpdk_engine_free(csilk_dpdk_engine_t* engine);

/**
 * @brief Poll mode receive raw ethernet mbuf packets from DPDK RX queue ring.
 * @param engine DPDK engine handle.
 * @param[out] pkt_data Receives pointer to packet payload.
 * @param[out] pkt_len Receives packet length in bytes.
 * @return 0 on success, -1 if no packet available.
 */
int csilk_dpdk_rx_burst(csilk_dpdk_engine_t* engine, const uint8_t** pkt_data, size_t* pkt_len);

/**
 * @brief Poll mode transmit raw ethernet mbuf packets to DPDK TX queue ring.
 * @param engine DPDK engine handle.
 * @param pkt_data Packet payload bytes to send.
 * @param pkt_len Packet length in bytes.
 * @return 0 on success, -1 on failure.
 */
int csilk_dpdk_tx_burst(csilk_dpdk_engine_t* engine, const uint8_t* pkt_data, size_t pkt_len);

#ifdef __cplusplus
}
#endif
