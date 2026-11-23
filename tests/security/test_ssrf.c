#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/security.h"
#include "csilk/csilk.h"

static void
test_ipv4_private_ranges(void)
{
    printf("Testing IPv4 private/loopback/metadata detection...\n");

    /* Loopback 127.0.0.0/8 */
    assert(csilk_security_is_private_ip("127.0.0.1") == true);
    assert(csilk_security_is_private_ip("127.255.0.1") == true);

    /* Private Class A 10.0.0.0/8 */
    assert(csilk_security_is_private_ip("10.0.0.1") == true);
    assert(csilk_security_is_private_ip("10.254.254.254") == true);

    /* Private Class B 172.16.0.0/12 */
    assert(csilk_security_is_private_ip("172.16.0.1") == true);
    assert(csilk_security_is_private_ip("172.31.255.254") == true);
    assert(csilk_security_is_private_ip("172.15.0.1") == false);
    assert(csilk_security_is_private_ip("172.32.0.1") == false);

    /* Private Class C 192.168.0.0/16 */
    assert(csilk_security_is_private_ip("192.168.0.1") == true);
    assert(csilk_security_is_private_ip("192.168.254.254") == true);
    assert(csilk_security_is_private_ip("192.169.1.1") == false);

    /* Link-Local / Cloud Metadata 169.254.0.0/16 */
    assert(csilk_security_is_private_ip("169.254.169.254") == true);
    assert(csilk_security_is_private_ip("169.254.1.1") == true);

    /* 0.0.0.0/8 and broadcast */
    assert(csilk_security_is_private_ip("0.0.0.0") == true);
    assert(csilk_security_is_private_ip("255.255.255.255") == true);

    /* Public IPs */
    assert(csilk_security_is_private_ip("8.8.8.8") == false);
    assert(csilk_security_is_private_ip("1.1.1.1") == false);
    assert(csilk_security_is_private_ip("93.184.216.34") == false);

    printf("  IPv4 ranges passed!\n");
}

static void
test_ipv6_private_ranges(void)
{
    printf("Testing IPv6 private/loopback/ULA detection...\n");

    /* Loopback & Unspecified */
    assert(csilk_security_is_private_ip("::1") == true);
    assert(csilk_security_is_private_ip("::") == true);

    /* Link-Local fe80::/10 */
    assert(csilk_security_is_private_ip("fe80::1") == true);
    assert(csilk_security_is_private_ip("febf::ffff") == true);

    /* Unique Local Address fc00::/7 */
    assert(csilk_security_is_private_ip("fc00::1") == true);
    assert(csilk_security_is_private_ip("fd12:3456:789a::1") == true);

    /* IPv4-mapped IPv6 */
    assert(csilk_security_is_private_ip("::ffff:127.0.0.1") == true);
    assert(csilk_security_is_private_ip("::ffff:169.254.169.254") == true);
    assert(csilk_security_is_private_ip("::ffff:10.0.0.1") == true);
    assert(csilk_security_is_private_ip("::ffff:8.8.8.8") == false);

    /* Public IPv6 */
    assert(csilk_security_is_private_ip("2001:4860:4860::8888") == false);
    assert(csilk_security_is_private_ip("2606:4700:4700::1111") == false);

    /* Edge cases */
    assert(csilk_security_is_private_ip(nullptr) == false);
    assert(csilk_security_is_private_ip("") == false);
    assert(csilk_security_is_private_ip("invalid-ip-string") == false);

    printf("  IPv6 ranges passed!\n");
}

static void
test_url_ssrf_safety(void)
{
    printf("Testing URL SSRF validation...\n");

    /* Cloud metadata exploitation */
    assert(csilk_security_url_is_ssrf_safe("http://169.254.169.254/latest/meta-data/", false) ==
           false);

    /* Internal service loopback exploitation */
    assert(csilk_security_url_is_ssrf_safe("http://127.0.0.1:8080/admin", false) == false);
    assert(csilk_security_url_is_ssrf_safe("http://localhost/admin", false) == false);
    assert(csilk_security_url_is_ssrf_safe("http://[::1]:8080/status", false) == false);

    /* Internal LAN access */
    assert(csilk_security_url_is_ssrf_safe("http://10.0.0.5:9200/_cat/indices", false) == false);
    assert(csilk_security_url_is_ssrf_safe("http://192.168.1.1/setup", false) == false);
    assert(csilk_security_url_is_ssrf_safe("http://172.20.0.2:8080/", false) == false);

    /* Safe public services */
    assert(csilk_security_url_is_ssrf_safe("https://api.openai.com/v1/chat/completions", false) ==
           true);
    assert(csilk_security_url_is_ssrf_safe("http://8.8.8.8/dns-query", false) == true);

    /* allow_private = true allows intranet calls */
    assert(csilk_security_url_is_ssrf_safe("http://127.0.0.1:8080/test", true) == true);
    assert(csilk_security_url_is_ssrf_safe("http://192.168.1.1/test", true) == true);

    /* Dangerous protocols blocked regardless */
    assert(csilk_security_url_is_ssrf_safe("file:///etc/passwd", false) == false);
    assert(csilk_security_url_is_ssrf_safe("file:///etc/passwd", true) == false);
    assert(csilk_security_url_is_ssrf_safe("gopher://127.0.0.1:6379/_flushall", false) == false);
    assert(csilk_security_url_is_ssrf_safe("ftp://127.0.0.1/data", false) == false);

    /* Malformed URLs */
    assert(csilk_security_url_is_ssrf_safe(nullptr, false) == false);
    assert(csilk_security_url_is_ssrf_safe("", false) == false);
    assert(csilk_security_url_is_ssrf_safe("http://", false) == false);

    printf("  URL SSRF validation passed!\n");
}

int
main(void)
{
    test_ipv4_private_ranges();
    test_ipv6_private_ranges();
    test_url_ssrf_safety();
    printf("All SSRF tests passed successfully!\n");
    return 0;
}
