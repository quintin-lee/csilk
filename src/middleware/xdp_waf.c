#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xdp_waf_internal.h"

csilk_xdp_waf_t*
csilk_xdp_waf_new(const char* ifname)
{
    csilk_xdp_waf_t* waf = calloc(1, sizeof(csilk_xdp_waf_t));
    if (!waf) {
        return NULL;
    }

    snprintf(waf->ifname, sizeof(waf->ifname), "%s", ifname ? ifname : "lo");
    waf->bpf_map_fd = -1;
    waf->is_kernel_attached = 0;
    csilk_mutex_init(&waf->mutex);
    return waf;
}

int
csilk_xdp_waf_add_ip_rule(csilk_xdp_waf_t* waf, const char* ip_cidr, uint8_t action)
{
    if (!waf || !ip_cidr) {
        return -1;
    }

    csilk_mutex_lock(&waf->mutex);

    if (waf->rule_count >= 256) {
        csilk_mutex_unlock(&waf->mutex);
        return -1;
    }

    char ip_str[64];
    snprintf(ip_str, sizeof(ip_str), "%s", ip_cidr);
    char*    slash = strchr(ip_str, '/');
    uint32_t prefix_len = 32;

    if (slash) {
        *slash = '\0';
        prefix_len = (uint32_t)atoi(slash + 1);
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        csilk_mutex_unlock(&waf->mutex);
        return -1;
    }

    csilk_xdp_ip_rule_t* rule = &waf->rules[waf->rule_count++];
    rule->ip = ntohl(addr.s_addr);
    rule->prefix_len = prefix_len;
    rule->action = action;

    csilk_mutex_unlock(&waf->mutex);
    return 0;
}

int
csilk_xdp_waf_block_ip(csilk_xdp_waf_t* waf, const char* ip_str)
{
    return csilk_xdp_waf_add_ip_rule(waf, ip_str, CSILK_XDP_ACTION_DROP);
}

int
csilk_xdp_waf_block_pattern(csilk_xdp_waf_t* waf, const char* pattern)
{
    if (!waf || !pattern) {
        return -1;
    }

    csilk_mutex_lock(&waf->mutex);
    if (waf->pattern_count >= 128) {
        csilk_mutex_unlock(&waf->mutex);
        return -1;
    }

    waf->patterns[waf->pattern_count] = strdup(pattern);
    if (!waf->patterns[waf->pattern_count]) {
        csilk_mutex_unlock(&waf->mutex);
        return -1;
    }

    waf->pattern_count++;
    csilk_mutex_unlock(&waf->mutex);
    return 0;
}

csilk_xdp_action_t
csilk_xdp_waf_inspect(csilk_xdp_waf_t* waf, uint32_t src_ip, const uint8_t* payload, size_t len)
{
    if (!waf) {
        return CSILK_XDP_ACTION_PASS;
    }

    csilk_mutex_lock(&waf->mutex);

    for (size_t i = 0; i < waf->rule_count; i++) {
        if (waf->rules[i].ip == src_ip && waf->rules[i].action == CSILK_XDP_ACTION_DROP) {
            csilk_mutex_unlock(&waf->mutex);
            return CSILK_XDP_ACTION_DROP;
        }
    }

    if (payload && len > 0) {
        for (size_t i = 0; i < waf->pattern_count; i++) {
            if (waf->patterns[i] &&
                memmem(payload, len, waf->patterns[i], strlen(waf->patterns[i]))) {
                csilk_mutex_unlock(&waf->mutex);
                return CSILK_XDP_ACTION_DROP;
            }
        }
    }

    csilk_mutex_unlock(&waf->mutex);
    return CSILK_XDP_ACTION_PASS;
}

void
csilk_xdp_waf_free(csilk_xdp_waf_t* waf)
{
    if (!waf) {
        return;
    }
    csilk_mutex_lock(&waf->mutex);
    for (size_t i = 0; i < waf->pattern_count; i++) {
        if (waf->patterns[i]) {
            free(waf->patterns[i]);
        }
    }
    csilk_mutex_unlock(&waf->mutex);
    csilk_mutex_destroy(&waf->mutex);
    free(waf);
}
