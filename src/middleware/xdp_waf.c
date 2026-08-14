/**
 * @file xdp_waf.c
 * @brief eBPF/XDP-based Web Application Firewall (WAF) implementation.
 *
 * Provides an in-memory rule engine for filtering network traffic by source
 * IP (IPv4 CIDR) and payload patterns. Rules are matched under a mutex so the
 * structure is safe to share across worker threads. This module maintains the
 * user-space view of rules; kernel attachment via the eBPF/XDP program is
 * optional and tracked by the is_kernel_attached flag.
 * @copyright MIT License
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xdp_waf_internal.h"

/**
 * @brief Create a new XDP WAF instance.
 *
 * Allocates and zero-initializes an XDP WAF handle bound to the given network
 * interface. The BPF map file descriptor is set to -1 (not attached) and the
 * rule/pattern counts start at zero.
 *
 * @param ifname  Network interface name to associate with the WAF (e.g. "eth0").
 *                May be NULL, in which case "lo" (loopback) is used.
 * @return Pointer to the newly allocated csilk_xdp_waf_t, or NULL on allocation
 *         failure. The caller must free it with csilk_xdp_waf_free().
 */
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

/**
 * @brief Add an IP-based rule (IP/prefix -> action) to the XDP WAF.
 *
 * Parses an IPv4 CIDR string (e.g. "10.0.0.0/8") and appends a rule to the
 * WAF's rule table. The source address is stored in host byte order.
 *
 * @param[in,out] waf       The XDP WAF instance. Must not be NULL.
 * @param[in]     ip_cidr   IPv4 CIDR string (address optional "/prefix").
 * @param[in]     action    Action to take when matched (e.g. DROP/PASS).
 * @return 0 on success, -1 on NULL input, full rule table (256), or invalid
 *         CIDR/prefix.
 */
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

/**
 * @brief Convenience wrapper to block an IP (drop action).
 *
 * Adds a DROP rule for the given IPv4 CIDR by delegating to
 * csilk_xdp_waf_add_ip_rule() with action CSILK_XDP_ACTION_DROP.
 *
 * @param[in,out] waf      The XDP WAF instance. Must not be NULL.
 * @param[in]     ip_str   IPv4 CIDR string to block. Must not be NULL.
 * @return 0 on success, -1 on error (see csilk_xdp_waf_add_ip_rule()).
 */
int
csilk_xdp_waf_block_ip(csilk_xdp_waf_t* waf, const char* ip_str)
{
    return csilk_xdp_waf_add_ip_rule(waf, ip_str, CSILK_XDP_ACTION_DROP);
}

/**
 * @brief Add a payload-substring block pattern to the XDP WAF.
 *
 * Duplicates and stores a textual pattern; any packet whose payload contains
 * the pattern (via memmem) will be dropped during inspection. Patterns are
 * matched under the WAF mutex.
 *
 * @param[in,out] waf      The XDP WAF instance. Must not be NULL.
 * @param[in]     pattern  Null-terminated substring to block. Must not be NULL.
 * @return 0 on success, -1 on NULL input, full pattern table (128), or
 *         allocation failure for the duplicated string.
 */
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

/**
 * @brief Inspect a packet against the WAF's IP and payload rules.
 *
 * First checks whether the source IPv4 address matches a DROP rule, then (if a
 * payload is supplied) whether any registered pattern appears in the payload.
 * Returns DROP if any match is found, otherwise PASS.
 *
 * @param[in] waf      The XDP WAF instance. If NULL, returns PASS.
 * @param[in] src_ip   Source IPv4 address in host byte order.
 * @param[in] payload  Packet payload bytes (may be NULL if len == 0).
 * @param[in] len      Length of the payload in bytes.
 * @return CSILK_XDP_ACTION_DROP if a rule/pattern matches, else
 *         CSILK_XDP_ACTION_PASS.
 */
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

/**
 * @brief Destroy an XDP WAF instance and free all resources.
 *
 * Frees each duplicated pattern string, destroys the protecting mutex, and
 * frees the WAF handle itself. Safe to call with NULL.
 *
 * @param[in,out] waf  The XDP WAF instance to free. May be NULL.
 */
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
