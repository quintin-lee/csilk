#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/io/af_xdp.h"

int
main(void)
{
    printf("Testing AF_XDP Kernel Bypass Zero-Copy Network Driver...\n");

    csilk_xdp_config_t cfg = {.ifname = "eth0", .queue_id = 0, .frame_size = 2048, .frame_num = 16};

    csilk_xdp_socket_t* xsk = csilk_xdp_socket_new(&cfg);
    assert(xsk != NULL);

    /* Test 1: TX packet burst */
    const char* pkt = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(csilk_xdp_tx_burst(xsk, (const uint8_t*)pkt, strlen(pkt)) == 0);

    /* Test 2: RX packet burst */
    const uint8_t* rx_buf = NULL;
    size_t         rx_len = 0;
    assert(csilk_xdp_rx_burst(xsk, &rx_buf, &rx_len) == 0);
    assert(rx_buf != NULL);
    assert(rx_len == 2048);

    csilk_xdp_socket_free(xsk);
    printf("test_af_xdp: PASS\n");
    return 0;
}
