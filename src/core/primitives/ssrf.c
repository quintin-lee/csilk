/**
 * @file ssrf.c
 * @brief Network address validation and SSRF protection engine.
 * @copyright MIT License
 */

#include "csilk/core/security.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
is_private_ipv4(uint32_t ip)
{
    /* 0.0.0.0/8 (Current network) */
    if ((ip >> 24) == 0) {
        return true;
    }

    /* 10.0.0.0/8 (Private Class A) */
    if ((ip >> 24) == 10) {
        return true;
    }

    /* 127.0.0.0/8 (Loopback) */
    if ((ip >> 24) == 127) {
        return true;
    }

    /* 169.254.0.0/16 (Link-Local & Cloud Metadata e.g. 169.254.169.254) */
    if ((ip & 0xFFFF0000U) == 0xA9FE0000U) {
        return true;
    }

    /* 172.16.0.0/12 (Private Class B: 172.16.0.0 - 172.31.255.255) */
    if ((ip & 0xFFF00000U) == 0xAC100000U) {
        return true;
    }

    /* 192.168.0.0/16 (Private Class C) */
    if ((ip & 0xFFFF0000U) == 0xC0A80000U) {
        return true;
    }

    /* 100.64.0.0/10 (Carrier-Grade NAT RFC 6598) */
    if ((ip & 0xFFC00000U) == 0x64400000U) {
        return true;
    }

    /* 255.255.255.255/32 (Broadcast) */
    if (ip == 0xFFFFFFFFU) {
        return true;
    }

    return false;
}

static bool
is_private_ipv6(const uint8_t* b)
{
    /* Unspecified address :: */
    static const uint8_t k_unspecified[16] = {0};
    if (memcmp(b, k_unspecified, 16) == 0) {
        return true;
    }

    /* Loopback address ::1 */
    static const uint8_t k_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(b, k_loopback, 16) == 0) {
        return true;
    }

    /* IPv4-mapped IPv6 ::ffff:x.x.x.x */
    static const uint8_t k_ipv4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(b, k_ipv4_mapped, 12) == 0) {
        uint32_t ip4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) |
                       (uint32_t)b[15];
        return is_private_ipv4(ip4);
    }

    /* Link-Local fe80::/10 */
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) {
        return true;
    }

    /* Unique Local Address (ULA) fc00::/7 */
    if ((b[0] & 0xFE) == 0xFC) {
        return true;
    }

    return false;
}

bool
csilk_security_is_private_ip(const char* ip_str)
{
    if (!ip_str || ip_str[0] == '\0') {
        return false;
    }

    /* Strip brackets if [IPv6] */
    char   clean_ip[128];
    size_t len = strlen(ip_str);
    if (ip_str[0] == '[' && ip_str[len - 1] == ']' && len >= 3 && len < sizeof(clean_ip)) {
        memcpy(clean_ip, ip_str + 1, len - 2);
        clean_ip[len - 2] = '\0';
    } else if (len < sizeof(clean_ip)) {
        memcpy(clean_ip, ip_str, len + 1);
    } else {
        return false;
    }

    /* 1. Try IPv4 */
    struct in_addr in4;
    if (inet_pton(AF_INET, clean_ip, &in4) == 1) {
        uint32_t ip = ntohl(in4.s_addr);
        return is_private_ipv4(ip);
    }

    /* 2. Try IPv6 */
    struct in6_addr in6;
    if (inet_pton(AF_INET6, clean_ip, &in6) == 1) {
        return is_private_ipv6(in6.s6_addr);
    }

    return false;
}

bool
csilk_security_url_is_ssrf_safe(const char* url, bool allow_private)
{
    if (!url || url[0] == '\0') {
        return false;
    }

    /* Validate scheme */
    const char* p = url;
    size_t      scheme_len = 0;
    if (strncasecmp(p, "http://", 7) == 0) {
        scheme_len = 7;
    } else if (strncasecmp(p, "https://", 8) == 0) {
        scheme_len = 8;
    } else {
        /* Reject non-HTTP protocols (e.g. file://, gopher://, ftp://) */
        return false;
    }

    const char* host_start = p + scheme_len;
    if (*host_start == '\0') {
        return false;
    }

    /* Find end of host/authority: '/', '?', '#', or end of string */
    const char* end = host_start;
    while (*end && *end != '/' && *end != '?' && *end != '#') {
        end++;
    }

    size_t auth_len = (size_t)(end - host_start);
    if (auth_len == 0 || auth_len >= 256) {
        return false;
    }

    char authority[256];
    memcpy(authority, host_start, auth_len);
    authority[auth_len] = '\0';

    /* Parse userinfo (user:pass@host) if present */
    char* at = strchr(authority, '@');
    char* host_part = at ? (at + 1) : authority;
    if (*host_part == '\0') {
        return false;
    }

    /* Handle IPv6 literal in authority: [::1]:port */
    char host[256];
    if (*host_part == '[') {
        char* closing = strchr(host_part, ']');
        if (!closing) {
            return false;
        }
        size_t ip6_len = (size_t)(closing - host_part + 1);
        if (ip6_len >= sizeof(host)) {
            return false;
        }
        memcpy(host, host_part, ip6_len);
        host[ip6_len] = '\0';
    } else {
        /* IPv4 or domain name, strip optional :port */
        char*  colon = strchr(host_part, ':');
        size_t h_len = colon ? (size_t)(colon - host_part) : strlen(host_part);
        if (h_len == 0 || h_len >= sizeof(host)) {
            return false;
        }
        memcpy(host, host_part, h_len);
        host[h_len] = '\0';
    }

    /* Check localhost aliases */
    if (strcasecmp(host, "localhost") == 0 ||
        (strlen(host) > 10 && strcasecmp(host + strlen(host) - 10, ".localhost") == 0)) {
        if (!allow_private) {
            return false;
        }
        return true;
    }

    /* Check if IP address */
    if (csilk_security_is_private_ip(host)) {
        if (!allow_private) {
            return false;
        }
    }

    return true;
}
