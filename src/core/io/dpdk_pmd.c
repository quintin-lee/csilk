/**
 * @file dpdk_pmd.c
 * @brief DPDK (Data Plane Development Kit) Poll Mode Driver implementation.
 * @copyright MIT License
 */

#include "csilk/core/dpdk_pmd.h"
#include <stdlib.h>
#include <string.h>

struct csilk_dpdk_engine_s {
    csilk_dpdk_config_t config;
    uint8_t*            mbuf_pool;
    size_t              pool_bytes;
    size_t              rx_head;
    size_t              tx_head;
};

/**
 * @brief Create a DPDK-style PMD engine from a configuration.
 * @param[in] config Engine configuration (validated non-NULL); zero pool_size/
 *                    mbuf_size default to 1024/2048.
 * @return A newly allocated engine with an aligned mbuf pool, or NULL on invalid
 *         config or allocation failure.
 * @note Copies the config (applying defaults), allocates a 64-byte aligned mbuf
 *       pool of pool_size*mbuf_size bytes, and initializes rx/tx cursors to 0.
 */
csilk_dpdk_engine_t*
csilk_dpdk_engine_new(const csilk_dpdk_config_t* config)
{
    if (!config) {
        return NULL;
    }

    csilk_dpdk_engine_t* engine = calloc(1, sizeof(csilk_dpdk_engine_t));
    if (!engine) {
        return NULL;
    }

    engine->config = *config;
    if (engine->config.pool_size == 0) {
        engine->config.pool_size = 1024;
    }
    if (engine->config.mbuf_size == 0) {
        engine->config.mbuf_size = 2048;
    }

    engine->pool_bytes = engine->config.pool_size * engine->config.mbuf_size;
    engine->mbuf_pool = aligned_alloc(64, engine->pool_bytes);
    if (!engine->mbuf_pool) {
        free(engine);
        return NULL;
    }

    memset(engine->mbuf_pool, 0, engine->pool_bytes);
    engine->rx_head = 0;
    engine->tx_head = 0;

    return engine;
}

/**
 * @brief Free a DPDK-style PMD engine and its mbuf pool.
 * @param[in] engine Engine to free (no-op if NULL).
 * @note Frees the mbuf pool (if any) and the engine struct.
 */
void
csilk_dpdk_engine_free(csilk_dpdk_engine_t* engine)
{
    if (!engine) {
        return;
    }
    if (engine->mbuf_pool) {
        free(engine->mbuf_pool);
    }
    free(engine);
}

/**
 * @brief Receive one packet buffer from the engine RX ring (single-burst).
 * @param[in]  engine   Engine to receive from (validated non-NULL).
 * @param[out] pkt_data Receives a pointer into the mbuf pool for the next packet.
 * @param[out] pkt_len  Receives the mbuf size.
 * @return 0 on success, -1 on NULL arguments.
 * @note Advances an internal RX cursor modulo pool size; the returned pointer
 *       aliases memory owned by the engine.
 */
int
csilk_dpdk_rx_burst(csilk_dpdk_engine_t* engine, const uint8_t** pkt_data, size_t* pkt_len)
{
    if (!engine || !pkt_data || !pkt_len) {
        return -1;
    }

    size_t   idx = engine->rx_head % engine->config.pool_size;
    uint8_t* ptr = engine->mbuf_pool + (idx * engine->config.mbuf_size);

    *pkt_data = ptr;
    *pkt_len = engine->config.mbuf_size;
    engine->rx_head++;

    return 0;
}

/**
 * @brief Transmit one packet into the engine TX ring (single-burst).
 * @param[in] engine   Engine to transmit on (validated non-NULL).
 * @param[in] pkt_data Packet bytes to copy into the mbuf pool.
 * @param[in] pkt_len  Length of pkt_data (must be <= configured mbuf size).
 * @return 0 on success, -1 on NULL args, zero length, or oversized packet.
 * @note Copies pkt_data into the next mbuf TX slot and advances the TX cursor.
 */
int
csilk_dpdk_tx_burst(csilk_dpdk_engine_t* engine, const uint8_t* pkt_data, size_t pkt_len)
{
    if (!engine || !pkt_data || pkt_len == 0) {
        return -1;
    }

    if (pkt_len > engine->config.mbuf_size) {
        return -1;
    }

    size_t   idx = engine->tx_head % engine->config.pool_size;
    uint8_t* ptr = engine->mbuf_pool + (idx * engine->config.mbuf_size);

    memcpy(ptr, pkt_data, pkt_len);
    engine->tx_head++;

    return 0;
}
