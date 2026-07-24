#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/dpdk_pmd.h"

int
main(void)
{
    printf("Testing DPDK Poll Mode Driver (PMD) Engine...\n");

    csilk_dpdk_config_t cfg = {
        .port_id = 0, .rx_rings = 1, .tx_rings = 1, .pool_size = 16, .mbuf_size = 2048};

    csilk_dpdk_engine_t* engine = csilk_dpdk_engine_new(&cfg);
    assert(engine != NULL);

    /* Test 1: TX burst */
    const char* pkt = "GET /api/v1/ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(csilk_dpdk_tx_burst(engine, (const uint8_t*)pkt, strlen(pkt)) == 0);

    /* Test 2: RX burst */
    const uint8_t* rx_data = NULL;
    size_t         rx_len = 0;
    assert(csilk_dpdk_rx_burst(engine, &rx_data, &rx_len) == 0);
    assert(rx_data != NULL);
    assert(rx_len == 2048);

    csilk_dpdk_engine_free(engine);
    printf("test_dpdk_pmd: PASS\n");
    return 0;
}
