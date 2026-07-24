#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/middleware/xdp_waf.h"

int
main(void)
{
    printf("Testing eBPF XDP Hardware Offload WAF...\n");

    csilk_xdp_waf_t* waf = csilk_xdp_waf_new();
    assert(waf != NULL);

    /* Test 1: Block IP */
    assert(csilk_xdp_waf_block_ip(waf, "192.168.1.100") == 0);

    struct in_addr addr1, addr2;
    inet_pton(AF_INET, "192.168.1.100", &addr1);
    inet_pton(AF_INET, "10.0.0.1", &addr2);

    const char*        clean_pkg = "GET /index.html HTTP/1.1\r\n";
    csilk_xdp_action_t act1 = csilk_xdp_waf_inspect(
        waf, ntohl(addr1.s_addr), (const uint8_t*)clean_pkg, strlen(clean_pkg));
    assert(act1 == CSILK_XDP_ACTION_DROP);

    csilk_xdp_action_t act2 = csilk_xdp_waf_inspect(
        waf, ntohl(addr2.s_addr), (const uint8_t*)clean_pkg, strlen(clean_pkg));
    assert(act2 == CSILK_XDP_ACTION_PASS);

    /* Test 2: Block Pattern */
    assert(csilk_xdp_waf_block_pattern(waf, "UNION SELECT") == 0);

    const char*        sqli_pkg = "GET /search?q=1 UNION SELECT * HTTP/1.1\r\n";
    csilk_xdp_action_t act3 =
        csilk_xdp_waf_inspect(waf, ntohl(addr2.s_addr), (const uint8_t*)sqli_pkg, strlen(sqli_pkg));
    assert(act3 == CSILK_XDP_ACTION_DROP);

    csilk_xdp_waf_free(waf);
    printf("test_xdp_waf: PASS\n");
    return 0;
}
