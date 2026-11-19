#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/primitives/codec.h"

#define PROP_ITERATIONS 10000

static uint64_t
xorshift64(uint64_t* state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static uint8_t
rand_byte(uint64_t* state)
{
    return (uint8_t)(xorshift64(state) & 0xFF);
}

static char
rand_hex_char(uint64_t* state)
{
    const char hex[] = "0123456789ABCDEFabcdef";
    return hex[xorshift64(state) % 22];
}

static char
rand_url_char(uint64_t* state)
{
    const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
                         "-._~:/?#[]@!$&'()*+,;=%";
    return chars[xorshift64(state) % (sizeof(chars) - 1)];
}

static void
test_url_decode_stable(void)
{
    printf("Testing URL decode round-trip...\n");
    uint64_t seed = 42;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        /* Generate a plain (no percent-encoding) string, verify decode
         * leaves it unchanged, then verify decode of decoded = decoded. */
        char buf[256];
        int  pos = 0;
        int  len = (int)(xorshift64(&seed) % 100);
        for (int j = 0; j < len && pos < 200; j++) {
            /* Plain alnum + space chars, no '%' so no expansion occurs */
            buf[pos++] = (char)('a' + (int)(xorshift64(&seed) % 26));
        }
        buf[pos] = '\0';
        size_t len1 = csilk_url_decode(buf, sizeof(buf));
        /* Second decode: no '%' left, so output is identical */
        char saved[256];
        memcpy(saved, buf, len1 + 1);
        size_t len2 = csilk_url_decode(buf, sizeof(buf));
        assert(len1 == len2);
        assert(memcmp(buf, saved, len1 + 1) == 0);
    }
    printf("test_url_decode_stable passed\n");
}

static void
test_url_decode_length(void)
{
    printf("Testing URL decode length property...\n");
    uint64_t seed = 123;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        char buf[512]; /* 3× slack: max input 128 → max output 384 + NUL */
        int  pos = 0;
        int  len = (int)(xorshift64(&seed) % 128);
        for (int j = 0; j < len && pos < 128; j++) {
            buf[pos++] = rand_url_char(&seed);
        }
        buf[pos] = '\0';
        size_t input_len = strlen(buf);
        size_t output_len = csilk_url_decode(buf, sizeof(buf));
        /* Output may be longer than input when invalid '%' are escaped
         * as "%25".  The maximum expansion is 3× (each '%' → 3 chars). */
        assert(output_len <= input_len * 3);
        assert(output_len < 512); /* buffer bound */
    }
    printf("test_url_decode_length passed\n");
}

static void
test_url_decode_plain_identity(void)
{
    printf("Testing URL decode identity for plain strings...\n");
    uint64_t seed = 999;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        char buf[256];
        int  pos = 0;
        int  len = (int)(xorshift64(&seed) % 200);
        for (int j = 0; j < len && pos < 255; j++) {
            char c;
            do {
                c = (char)(xorshift64(&seed) & 0x7F);
            } while (c == '%' || c == '+');
            if (c >= 0x20) {
                buf[pos++] = c;
            }
        }
        buf[pos] = '\0';
        char original[256];
        strcpy(original, buf);
        size_t output_len = csilk_url_decode(buf, sizeof(buf));
        assert(output_len == pos);
        assert(strcmp(buf, original) == 0);
    }
    printf("test_url_decode_plain_identity passed\n");
}

static void
test_base64url_roundtrip(void)
{
    printf("Testing Base64URL roundtrip...\n");
    uint64_t seed = 777;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t src[256];
        size_t  len = xorshift64(&seed) % 200;
        for (size_t j = 0; j < len; j++) {
            src[j] = rand_byte(&seed);
        }
        char encoded[512];
        csilk_base64url_encode(src, len, encoded);
        uint8_t decoded[256];
        int     dlen = csilk_base64url_decode(encoded, decoded, sizeof(decoded));
        assert(dlen >= 0);
        assert((size_t)dlen == len);
        assert(memcmp(src, decoded, len) == 0);
    }
    printf("test_base64url_roundtrip passed\n");
}

static void
test_base64url_roundtrip_edge(void)
{
    printf("Testing Base64URL roundtrip edge lengths...\n");
    uint64_t seed = 555;
    for (int i = 0; i < 200; i++) {
        uint8_t src[8];
        size_t  len = (size_t)(xorshift64(&seed) % 8);
        for (size_t j = 0; j < len; j++) {
            src[j] = rand_byte(&seed);
        }
        char encoded[32];
        csilk_base64url_encode(src, len, encoded);
        uint8_t decoded[8];
        int     dlen = csilk_base64url_decode(encoded, decoded, sizeof(decoded));
        assert(dlen >= 0);
        assert((size_t)dlen == len);
        assert(memcmp(src, decoded, len) == 0);
    }

    uint8_t empty_src[] = "";
    char    encoded[8];
    csilk_base64url_encode(empty_src, 0, encoded);
    assert(strcmp(encoded, "") == 0);

    uint8_t empty_dst[8];
    assert(csilk_base64url_decode("", empty_dst, sizeof(empty_dst)) == 0);

    printf("test_base64url_roundtrip_edge passed\n");
}

static void
test_base64url_encode_charset(void)
{
    printf("Testing Base64URL encode charset...\n");
    uint64_t seed = 333;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t src[64];
        size_t  len = xorshift64(&seed) % 64;
        for (size_t j = 0; j < len; j++) {
            src[j] = rand_byte(&seed);
        }
        char encoded[128];
        csilk_base64url_encode(src, len, encoded);
        for (const char* p = encoded; *p; p++) {
            char c = *p;
            int  ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_';
            assert(ok);
        }
    }
    printf("test_base64url_encode_charset passed\n");
}

static void
test_base64url_no_padding(void)
{
    printf("Testing Base64URL no padding...\n");
    uint64_t seed = 444;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t src[64];
        size_t  len = xorshift64(&seed) % 64;
        for (size_t j = 0; j < len; j++) {
            src[j] = rand_byte(&seed);
        }
        char encoded[128];
        csilk_base64url_encode(src, len, encoded);
        assert(strchr(encoded, '=') == nullptr);
    }
    printf("test_base64url_no_padding passed\n");
}

static void
test_base64url_decode_random(void)
{
    printf("Testing Base64URL decode random inputs...\n");
    uint64_t seed = 888;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        char input[128];
        int  len = (int)(xorshift64(&seed) % 100);
        for (int j = 0; j < len; j++) {
            input[j] = (char)(rand_byte(&seed) & 0x7F);
        }
        input[len] = '\0';
        uint8_t out[128];
        int     result = csilk_base64url_decode(input, out, sizeof(out));
        if (result >= 0) {
            assert((size_t)result <= strlen(input) * 3 / 4 + 4);
        }
    }
    printf("test_base64url_decode_random passed\n");
}

int
main(void)
{
    test_url_decode_stable();
    test_url_decode_length();
    test_url_decode_plain_identity();
    test_base64url_roundtrip();
    test_base64url_roundtrip_edge();
    test_base64url_encode_charset();
    test_base64url_no_padding();
    test_base64url_decode_random();
    printf("test_codec_prop: ALL PASSED\n");
    return 0;
}
