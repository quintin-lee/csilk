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

    assert(csilk_xdp_waf_add_ip_rule(waf, "192.168.1.0/24", 1) == 0);
    assert(csilk_xdp_waf_add_ip_rule(waf, "10.0.0.1", 2) == 0);
    assert(csilk_xdp_waf_add_ip_rule(waf, "invalid_ip", 1) == -1);

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
