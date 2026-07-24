#pragma once
/**
 * @file xdp_waf.h
 * @brief eBPF XDP hardware offload WAF and IP blacklisting filter.
 *
 * @version 0.5.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_xdp_waf_s csilk_xdp_waf_t;

/**
 * @brief Action result returned by XDP packet inspection.
 */
typedef enum {
    CSILK_XDP_ACTION_PASS = 0, /**< Allow packet to pass to network stack. */
    CSILK_XDP_ACTION_DROP = 1, /**< Immediately drop packet in NIC driver. */
} csilk_xdp_action_t;

/**
 * @brief Create a new eBPF XDP WAF filter instance.
 * @return New instance, or nullptr on allocation failure.
 */
csilk_xdp_waf_t* csilk_xdp_waf_new(void);

/**
 * @brief Destroy an XDP WAF filter instance.
 * @param waf Instance to free.
 */
void csilk_xdp_waf_free(csilk_xdp_waf_t* waf);

/**
 * @brief Add an IPv4 address to the XDP hardware drop map.
 * @param waf XDP WAF instance.
 * @param ip_str IPv4 address string (e.g. "192.168.1.100").
 * @return 0 on success, -1 on invalid IP or map limit.
 */
int csilk_xdp_waf_block_ip(csilk_xdp_waf_t* waf, const char* ip_str);

/**
 * @brief Add a payload pattern match rule for XDP packet inspection.
 * @param waf XDP WAF instance.
 * @param pattern Raw byte or string pattern to drop (e.g. "UNION SELECT").
 * @return 0 on success, -1 on failure.
 */
int csilk_xdp_waf_block_pattern(csilk_xdp_waf_t* waf, const char* pattern);

/**
 * @brief Inspect an incoming raw ethernet frame and return XDP action.
 * @param waf XDP WAF instance.
 * @param src_ip IPv4 source address in host byte order.
 * @param payload Raw payload bytes.
 * @param len Payload length in bytes.
 * @return CSILK_XDP_ACTION_DROP if packet matches a block rule, CSILK_XDP_ACTION_PASS otherwise.
 */
csilk_xdp_action_t
csilk_xdp_waf_inspect(csilk_xdp_waf_t* waf, uint32_t src_ip, const uint8_t* payload, size_t len);

#ifdef __cplusplus
}
#endif
