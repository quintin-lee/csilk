#pragma once
/**
 * @file security.h
 * @brief Network and SSRF security utilities for csilk.
 *
 * Provides IP range validation (private/internal/loopback/cloud metadata)
 * and URL SSRF filtering.
 *
 * @copyright MIT License
 */

#include "csilk/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Checks whether an IP string is a private, loopback, link-local,
 * broadcast, or cloud provider metadata IP address.
 *
 * Validates:
 * - IPv4: 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12,
 *   192.168.0.0/16, 255.255.255.255
 * - IPv6: :: (unspecified), ::1 (loopback), fe80::/10 (link-local),
 *   fc00::/7 (unique local address), ::ffff:0:0/96 (IPv4-mapped)
 *
 * @param ip_str IPv4 or IPv6 address string.
 * @return true if the IP is private or restricted; false if public or invalid.
 */
bool csilk_security_is_private_ip(const char* ip_str);

/**
 * @brief Validates whether a target URL is safe against SSRF attacks.
 *
 * Checks that the scheme is http or https and that the host is not
 * a private/internal IP or localhost (unless allow_private is true).
 * Dangerous protocols such as file://, gopher://, ftp:// are always rejected.
 *
 * @param url The full URL string to check.
 * @param allow_private If true, private/internal IPs are permitted.
 * @return true if URL is considered safe; false otherwise.
 */
bool csilk_security_url_is_ssrf_safe(const char* url, bool allow_private);

#ifdef __cplusplus
}
#endif
