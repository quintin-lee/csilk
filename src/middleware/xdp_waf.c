/**
 * @file xdp_waf.c
 * @brief eBPF XDP hardware offload WAF and IP blacklisting filter implementation.
 * @copyright MIT License
 */

#include "csilk/middleware/xdp_waf.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BLOCKED_IPS 1024
#define MAX_PATTERNS 128

struct csilk_xdp_waf_s {
    uint32_t blocked_ips[MAX_BLOCKED_IPS];
    size_t   blocked_ip_count;
    char*    patterns[MAX_PATTERNS];
    size_t   pattern_count;
};

csilk_xdp_waf_t*
csilk_xdp_waf_new(void)
{
    csilk_xdp_waf_t* waf = calloc(1, sizeof(csilk_xdp_waf_t));
    return waf;
}

void
csilk_xdp_waf_free(csilk_xdp_waf_t* waf)
{
    if (!waf) {
        return;
    }
    for (size_t i = 0; i < waf->pattern_count; i++) {
        if (waf->patterns[i]) {
            free(waf->patterns[i]);
        }
    }
    free(waf);
}

int
csilk_xdp_waf_block_ip(csilk_xdp_waf_t* waf, const char* ip_str)
{
    if (!waf || !ip_str || waf->blocked_ip_count >= MAX_BLOCKED_IPS) {
        return -1;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return -1;
    }

    waf->blocked_ips[waf->blocked_ip_count++] = ntohl(addr.s_addr);
    return 0;
}

int
csilk_xdp_waf_block_pattern(csilk_xdp_waf_t* waf, const char* pattern)
{
    if (!waf || !pattern || waf->pattern_count >= MAX_PATTERNS) {
        return -1;
    }

    waf->patterns[waf->pattern_count] = strdup(pattern);
    if (!waf->patterns[waf->pattern_count]) {
        return -1;
    }
    waf->pattern_count++;
    return 0;
}

csilk_xdp_action_t
csilk_xdp_waf_inspect(csilk_xdp_waf_t* waf, uint32_t src_ip, const uint8_t* payload, size_t len)
{
    if (!waf) {
        return CSILK_XDP_ACTION_PASS;
    }

    /* 1. Check IP blacklist map */
    for (size_t i = 0; i < waf->blocked_ip_count; i++) {
        if (waf->blocked_ips[i] == src_ip) {
            return CSILK_XDP_ACTION_DROP;
        }
    }

    /* 2. Check Payload pattern rules */
    if (payload && len > 0) {
        for (size_t i = 0; i < waf->pattern_count; i++) {
            if (waf->patterns[i] &&
                memmem(payload, len, waf->patterns[i], strlen(waf->patterns[i]))) {
                return CSILK_XDP_ACTION_DROP;
            }
        }
    }

    return CSILK_XDP_ACTION_PASS;
}
