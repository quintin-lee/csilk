/**
 * @file xdp_waf_internal.h
 * @brief Internal header for eBPF XDP BPF-Maps and rules.
 * @copyright MIT License
 */

#ifndef CSILK_XDP_WAF_INTERNAL_H
#define CSILK_XDP_WAF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/sync.h"
#include "csilk/middleware/xdp_waf.h"

typedef struct {
    uint32_t ip;
    uint32_t prefix_len;
    uint8_t  action;
} csilk_xdp_ip_rule_t;

struct csilk_xdp_waf_s {
    char                ifname[16];
    int                 bpf_map_fd;
    int                 is_kernel_attached;
    csilk_xdp_ip_rule_t rules[256];
    size_t              rule_count;
    char*               patterns[128];
    size_t              pattern_count;
    csilk_mutex_t       mutex;
};

#endif /* CSILK_XDP_WAF_INTERNAL_H */
