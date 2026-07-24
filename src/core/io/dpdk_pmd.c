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
