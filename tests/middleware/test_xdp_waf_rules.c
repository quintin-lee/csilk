#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/middleware/xdp_waf.h"

static void
test_xdp_waf_rule_management(void)
{
    csilk_xdp_waf_t* waf = csilk_xdp_waf_new("eth0");
    assert(waf != nullptr);

    assert(csilk_xdp_waf_add_ip_rule(waf, "192.168.1.0/24", CSILK_XDP_ACTION_DROP) == 0);
    assert(csilk_xdp_waf_add_ip_rule(waf, "10.0.0.1", 2) == 0);
    assert(csilk_xdp_waf_add_ip_rule(waf, "invalid_ip", 1) == -1);
    assert(csilk_xdp_waf_add_ip_rule(waf, "10.0.0.0/33", 1) == -1);

    struct in_addr subnet_hit, subnet_miss;
    inet_pton(AF_INET, "192.168.1.42", &subnet_hit);
    inet_pton(AF_INET, "192.168.2.42", &subnet_miss);
    assert(csilk_xdp_waf_inspect(waf, ntohl(subnet_hit.s_addr), NULL, 0) == CSILK_XDP_ACTION_DROP);
    assert(csilk_xdp_waf_inspect(waf, ntohl(subnet_miss.s_addr), NULL, 0) == CSILK_XDP_ACTION_PASS);

    csilk_xdp_waf_free(waf);
    printf("test_xdp_waf_rule_management passed\n");
}

int
main(void)
{
    test_xdp_waf_rule_management();
    printf("All test_xdp_waf_rules tests passed successfully!\n");
    return 0;
}
