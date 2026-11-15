#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/io/af_xdp.h"

/*
 * AF_XDP driver test.
 *
 * Since v1.1, csilk_xdp_socket_new always allocates the socket struct and the
 * UMEM ring (cursor-simulation fallback). On Linux it additionally attempts a
 * real AF_XDP socket (socket/ioctl/mmap) which fails gracefully without
 * CAP_NET_RAW, falling back to the in-memory cursor path. TX/RX burst calls
 * operate on the UMEM ring regardless of whether the kernel socket succeeded.
 *
 * This test verifies the fallback cursor path works on all platforms.
 */
int
main(void)
{
    printf("Testing AF_XDP Kernel Bypass Zero-Copy Network Driver...\n");

    csilk_xdp_config_t cfg = {.ifname = "eth0", .queue_id = 0, .frame_size = 2048, .frame_num = 16};

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

    /* NULL safety */
    assert(csilk_xdp_tx_burst(NULL, NULL, 0) == -1);
    assert(csilk_xdp_rx_burst(NULL, NULL, NULL) == -1);

    csilk_xdp_socket_free(xsk);
    printf("test_af_xdp: PASS\n");
    return 0;
}
