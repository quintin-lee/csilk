#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/io/af_xdp.h"

/*
 * AF_XDP driver test.
 *
 * Verifies NULL-safety guards and real Linux AF_XDP (XSK) behavior when
 * CAP_NET_RAW / kernel support is available, or clean fail-closed behavior
 * (NULL return with non-zero errno) otherwise.
 */
int
main(void)
{
    printf("Testing AF_XDP Kernel Bypass Zero-Copy Network Driver...\n");

    const char* pkt = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

    /* NULL safety & error handling */
    assert(csilk_xdp_socket_new(NULL) == NULL);
    assert(csilk_xdp_tx_burst(NULL, NULL, 0) == -1);
    assert(csilk_xdp_rx_burst(NULL, NULL, NULL) == -1);
    assert(csilk_xdp_rx_burst_batch(NULL, NULL, NULL, 0) == -1);
    assert(csilk_xdp_fill_ring_produce(NULL, 0) == -1);
    assert(csilk_xdp_complete_ring_reap(NULL, NULL) == -1);
    assert(csilk_xdp_get_stats(NULL, NULL) == -1);
    assert(csilk_xdp_is_real_socket(NULL) == false);
    assert(csilk_xdp_get_fd(NULL) == -1);
    csilk_xdp_socket_free(NULL);

    csilk_xdp_config_t cfg = {
        .ifname = "lo",
        .queue_id = 0,
        .frame_size = 2048,
        .frame_num = 16,
        .xdp_flags = 0,
        .bind_flags = 0,
    };

    errno = 0;
    csilk_xdp_socket_t* xsk = csilk_xdp_socket_new(&cfg);
    if (!xsk) {
        assert(errno != 0);
        printf("test_af_xdp (fail-closed, errno=%d): PASS\n", errno);
        return 0;
    }

    assert(csilk_xdp_is_real_socket(xsk) == true);
    assert(csilk_xdp_get_fd(xsk) >= 0);
    assert(csilk_xdp_tx_burst(xsk, (const uint8_t*)pkt, 4096) == -1); /* oversized */

    csilk_xdp_stats_t stats = {0};
    assert(csilk_xdp_get_stats(xsk, &stats) == 0);

    csilk_xdp_socket_free(xsk);
    printf("test_af_xdp (real socket): PASS\n");
    return 0;
}
