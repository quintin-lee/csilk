/**
 * @file xdp_waf.h
 * @brief Dynamic eBPF XDP WAF security subsystem for csilk.
 * @copyright MIT License
 */

#ifndef CSILK_XDP_WAF_H
#define CSILK_XDP_WAF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_xdp_waf_s csilk_xdp_waf_t;

typedef enum {
    CSILK_XDP_ACTION_PASS = 0,
    CSILK_XDP_ACTION_DROP = 1,
} csilk_xdp_action_t;

/**
 * @brief Creates and binds an eBPF XDP WAF instance.
 * @param ifname Network interface name (e.g. "eth0", "lo", or NULL).
 * @return WAF handle or NULL on error.
 */
csilk_xdp_waf_t* csilk_xdp_waf_new(const char* ifname);

/**
 * @brief Dynamically adds an IP/CIDR filtering rule (triggers zero-downtime BPF-Map update).
 * @param waf WAF handle.
 * @param ip_cidr IP CIDR string (e.g., "192.168.1.0/24" or "10.0.0.1").
 * @param action Action code (0: PASS, 1: DROP, 2: RATELIMIT).
 * @return 0 on success, negative value on error.
 */
int csilk_xdp_waf_add_ip_rule(csilk_xdp_waf_t* waf, const char* ip_cidr, uint8_t action);

/**
 * @brief Add an IPv4 address to the XDP hardware drop map.
 */
int csilk_xdp_waf_block_ip(csilk_xdp_waf_t* waf, const char* ip_str);

/**
 * @brief Add a payload pattern match rule for XDP packet inspection.
 */
int csilk_xdp_waf_block_pattern(csilk_xdp_waf_t* waf, const char* pattern);

/**
 * @brief Inspect an incoming raw ethernet frame and return XDP action.
 */
csilk_xdp_action_t
csilk_xdp_waf_inspect(csilk_xdp_waf_t* waf, uint32_t src_ip, const uint8_t* payload, size_t len);

/**
 * @brief Frees eBPF XDP WAF instance.
 * @param waf WAF handle to release.
 */
void csilk_xdp_waf_free(csilk_xdp_waf_t* waf);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_XDP_WAF_H */
