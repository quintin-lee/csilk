#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/middleware.h"

static void
test_waf_filter_ip_and_pattern(void)
{
    printf("Testing userspace IP/CIDR and payload WAF filter...\n");

    csilk_waf_filter_t* filter = csilk_waf_filter_new();
    assert(filter != NULL);

    /* Null safety */
    assert(csilk_waf_filter_add_ip_rule(NULL, "10.0.0.1", CSILK_WAF_ACTION_DROP) == -1);
    assert(csilk_waf_filter_add_ip_rule(filter, NULL, CSILK_WAF_ACTION_DROP) == -1);
    assert(csilk_waf_filter_block_pattern(NULL, "SELECT") == -1);
    assert(csilk_waf_filter_block_pattern(filter, NULL) == -1);
    assert(csilk_waf_filter_inspect(NULL, 0, NULL, 0) == CSILK_WAF_ACTION_PASS);

    /* Block IP */
    assert(csilk_waf_filter_block_ip(filter, "192.168.1.100") == 0);

    struct in_addr addr1, addr2;
    inet_pton(AF_INET, "192.168.1.100", &addr1);
    inet_pton(AF_INET, "10.0.0.1", &addr2);

    const char*        clean_pkg = "GET /index.html HTTP/1.1\r\n";
    csilk_waf_action_t act1 = csilk_waf_filter_inspect(
        filter, ntohl(addr1.s_addr), (const uint8_t*)clean_pkg, strlen(clean_pkg));
    assert(act1 == CSILK_WAF_ACTION_DROP);

    csilk_waf_action_t act2 = csilk_waf_filter_inspect(
        filter, ntohl(addr2.s_addr), (const uint8_t*)clean_pkg, strlen(clean_pkg));
    assert(act2 == CSILK_WAF_ACTION_PASS);

    /* Block Pattern */
    assert(csilk_waf_filter_block_pattern(filter, "UNION SELECT") == 0);

    const char*        sqli_pkg = "GET /search?q=1 UNION SELECT * HTTP/1.1\r\n";
    csilk_waf_action_t act3 = csilk_waf_filter_inspect(
        filter, ntohl(addr2.s_addr), (const uint8_t*)sqli_pkg, strlen(sqli_pkg));
    assert(act3 == CSILK_WAF_ACTION_DROP);

    csilk_waf_filter_free(filter);
    csilk_waf_filter_free(NULL);
    printf("test_waf_filter_ip_and_pattern: PASS\n");
}

static void
test_waf_filter_cidr_rules(void)
{
    csilk_waf_filter_t* filter = csilk_waf_filter_new();
    assert(filter != NULL);

    assert(csilk_waf_filter_add_ip_rule(filter, "192.168.1.0/24", CSILK_WAF_ACTION_DROP) == 0);
    assert(csilk_waf_filter_add_ip_rule(filter, "10.0.0.1", CSILK_WAF_ACTION_PASS) == 0);
    assert(csilk_waf_filter_add_ip_rule(filter, "invalid_ip", CSILK_WAF_ACTION_DROP) == -1);
    assert(csilk_waf_filter_add_ip_rule(filter, "10.0.0.0/33", CSILK_WAF_ACTION_DROP) == -1);
    assert(csilk_waf_filter_add_ip_rule(filter, "10.0.0.0/-1", CSILK_WAF_ACTION_DROP) == -1);
    assert(csilk_waf_filter_add_ip_rule(filter, "10.0.0.0/abc", CSILK_WAF_ACTION_DROP) == -1);

    struct in_addr subnet_hit, subnet_miss;
    inet_pton(AF_INET, "192.168.1.42", &subnet_hit);
    inet_pton(AF_INET, "192.168.2.42", &subnet_miss);
    assert(csilk_waf_filter_inspect(filter, ntohl(subnet_hit.s_addr), NULL, 0) ==
           CSILK_WAF_ACTION_DROP);
    assert(csilk_waf_filter_inspect(filter, ntohl(subnet_miss.s_addr), NULL, 0) ==
           CSILK_WAF_ACTION_PASS);

    csilk_waf_filter_free(filter);
    printf("test_waf_filter_cidr_rules: PASS\n");
}

int
main(void)
{
    test_waf_filter_ip_and_pattern();
    test_waf_filter_cidr_rules();
    printf("All test_waf_filter tests passed!\n");
    return 0;
}
