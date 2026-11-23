#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/io/af_xdp.h"

/*
 * AF_XDP driver test.
 *
 * Verifies real Linux AF_XDP (XSK) 4-ring interactions and seamless
 * fallback to high-performance in-memory simulation when running without
 * root/CAP_NET_RAW.
 */
int
main(void)
{
    printf("Testing AF_XDP Kernel Bypass Zero-Copy Network Driver...\n");

    csilk_xdp_config_t cfg = {
        .ifname = "eth0",
        .queue_id = 0,
        .frame_size = 2048,
        .frame_num = 16,
        .xdp_flags = 0,
        .bind_flags = 0,
    };

    csilk_xdp_socket_t* xsk = csilk_xdp_socket_new(&cfg);
    assert(xsk != NULL);

    /* TX packet burst — copies into UMEM ring, advances TX cursor */
    const char* pkt = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(csilk_xdp_tx_burst(xsk, (const uint8_t*)pkt, strlen(pkt)) == 0);

    /* RX packet burst — returns pointer into UMEM ring */
    const uint8_t* rx_buf = NULL;
    size_t         rx_len = 0;
    assert(csilk_xdp_rx_burst(xsk, &rx_buf, &rx_len) == 0);
    assert(rx_buf != NULL);
    assert(rx_len == 2048);

    /* Batch RX */
    const uint8_t* rx_arr[4] = {NULL};
    size_t         len_arr[4] = {0};
    int            batch_rc = csilk_xdp_rx_burst_batch(xsk, rx_arr, len_arr, 4);
    assert(batch_rc == 4);
    for (int i = 0; i < 4; i++) {
        assert(rx_arr[i] != NULL);
        assert(len_arr[i] == 2048);
    }

    /* Fill Ring and Completion Ring */
    assert(csilk_xdp_fill_ring_produce(xsk, 0x1000) == 0);
    uint64_t reaped_addr = 0;
    assert(csilk_xdp_complete_ring_reap(xsk, &reaped_addr) == 0);

    /* Statistics verification */
    csilk_xdp_stats_t stats = {0};
    assert(csilk_xdp_get_stats(xsk, &stats) == 0);
    assert(stats.tx_packets >= 1);
    assert(stats.rx_packets >= 5);
    assert(stats.tx_bytes >= strlen(pkt));
    assert(stats.rx_bytes >= 5 * 2048);

    /* Real socket and FD queries */
    bool is_real = csilk_xdp_is_real_socket(xsk);
    int  fd = csilk_xdp_get_fd(xsk);
    if (is_real) {
        assert(fd >= 0);
    } else {
        assert(fd == -1);
    }

    /* NULL safety & error handling */
    assert(csilk_xdp_tx_burst(NULL, NULL, 0) == -1);
    assert(csilk_xdp_tx_burst(xsk, (const uint8_t*)pkt, 4096) == -1); /* oversized */
    assert(csilk_xdp_rx_burst(NULL, NULL, NULL) == -1);
    assert(csilk_xdp_rx_burst_batch(NULL, NULL, NULL, 0) == -1);
    assert(csilk_xdp_fill_ring_produce(NULL, 0) == -1);
    assert(csilk_xdp_complete_ring_reap(NULL, NULL) == -1);
    assert(csilk_xdp_get_stats(NULL, NULL) == -1);
    assert(csilk_xdp_is_real_socket(NULL) == false);
    assert(csilk_xdp_get_fd(NULL) == -1);

    csilk_xdp_socket_free(xsk);
    csilk_xdp_socket_free(NULL);

    printf("test_af_xdp: PASS\n");
    return 0;
}
