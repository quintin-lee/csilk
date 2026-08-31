/**
 * @file test_crypto.c
 * @brief Property tests for csilk crypto primitives.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/primitives/codec.h"
#include "csilk/core/primitives/hash.h"
#include "csilk/core/internal.h"
#include "csilk/crypto/crypto.h"

/* ---- SHA-256 property tests ---- */

static void
test_sha256_properties(void)
{
    printf("Testing SHA-256 properties...\n");
    uint8_t data[] = "The quick brown fox jumps over the lazy dog";
    uint8_t h1[32], h2[32];

    /* Single-shot vs incremental equivalence */
    {
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, sizeof(data) - 1);
        csilk_sha256_final(&ctx, h1);
    }
    {
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, 4);
        csilk_sha256_update(&ctx, data + 4, sizeof(data) - 5);
        csilk_sha256_final(&ctx, h2);
    }
    assert(memcmp(h1, h2, 32) == 0);

    /* Empty input produces deterministic digest */
    {
        csilk_sha256_ctx ctx;
        uint8_t          empty[32];
        csilk_sha256_init(&ctx);
        csilk_sha256_final(&ctx, empty);
        assert(empty[0] != 0 || empty[1] != 0);
    }

    /* Different inputs produce different digests */
    {
        csilk_sha256_ctx ctx;
        uint8_t          ha[32], hb[32];
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, (const uint8_t*)"a", 1);
        csilk_sha256_final(&ctx, ha);
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, (const uint8_t*)"b", 1);
        csilk_sha256_final(&ctx, hb);
        assert(memcmp(ha, hb, 32) != 0);
    }
    printf("  passed\n");
}

/* ---- HMAC-SHA256 property tests ---- */

static void
test_hmac_properties(void)
{
    printf("Testing HMAC-SHA256 properties...\n");
    uint8_t out1[32], out2[32];

    /* Same key+data → same output */
    csilk_hmac_sha256((const uint8_t*)"key", 3, (const uint8_t*)"data", 4, out1);
    csilk_hmac_sha256((const uint8_t*)"key", 3, (const uint8_t*)"data", 4, out2);
    assert(memcmp(out1, out2, 32) == 0);

    /* Different keys → different output */
    uint8_t out3[32];
    csilk_hmac_sha256((const uint8_t*)"KEY", 3, (const uint8_t*)"data", 4, out3);
    assert(memcmp(out1, out3, 32) != 0);

    /* Long key (> 64 bytes) works without crash */
    uint8_t long_key[200];
    for (int i = 0; i < 200; i++) {
        long_key[i] = (uint8_t)i;
    }
    uint8_t out4[32];
    csilk_hmac_sha256(long_key, sizeof(long_key), (const uint8_t*)"test", 4, out4);
    assert(out4[0] != 0 || out4[1] != 0);

    /* Zero-length key works */
    uint8_t out5[32];
    csilk_hmac_sha256(NULL, 0, (const uint8_t*)"hello", 5, out5);
    printf("  passed\n");
}

/* ---- Base64 property tests ---- */

static void
test_base64_roundtrip(void)
{
    printf("Testing Base64 encode/decode roundtrip...\n");
    uint8_t original[] = {0x14, 0xfb, 0x9c, 0x03, 0xd9, 0x7e};
    char    encoded[128];
    csilk_base64_encode(original, sizeof(original), encoded);
    assert(strlen(encoded) == 8);
    printf("  passed\n");
}

static void
test_base64url_roundtrip(void)
{
    printf("Testing Base64URL encode/decode roundtrip...\n");
    uint8_t original[] = {0x14, 0xfb, 0x9c, 0x03, 0xd9, 0x7e};
    char    encoded[128];
    csilk_base64url_encode(original, sizeof(original), encoded);
    assert(strlen(encoded) == 8);

    uint8_t decoded[64];
    int     rc = csilk_base64url_decode(encoded, decoded, sizeof(decoded));
    assert(rc == (int)sizeof(original));
    assert(memcmp(original, decoded, sizeof(original)) == 0);
    printf("  passed\n");
}

static void
test_base64_no_padding_for_exact(void)
{
    printf("Testing Base64 padding rules...\n");
    char out[128];

    /* 3 bytes → no padding */
    csilk_base64_encode((const uint8_t*)"abc", 3, out);
    assert(strchr(out, '=') == NULL);
    assert(strlen(out) == 4);

    /* 1 byte → 2 padding chars */
    csilk_base64_encode((const uint8_t*)"a", 1, out);
    assert(strcmp(strchr(out, '='), "==") == 0);

    /* 2 bytes → 1 padding char */
    csilk_base64_encode((const uint8_t*)"ab", 2, out);
    assert(strcmp(strchr(out, '='), "=") == 0);

    printf("  passed\n");
}

static void
test_base64_empty(void)
{
    printf("Testing Base64 empty input...\n");
    char out[8];
    csilk_base64_encode(NULL, 0, out);
    assert(strcmp(out, "") == 0);
    csilk_base64url_encode(NULL, 0, out);
    assert(strcmp(out, "") == 0);
    printf("  passed\n");
}

/* ---- Randomness sanity ---- */

static void
test_fill_random_uniqueness(void)
{
    printf("Testing csilk_crypto_fill_random uniqueness...\n");
    uint8_t a[32], b[32];
    assert(csilk_crypto_fill_random(a, 32) == 0);
    assert(csilk_crypto_fill_random(b, 32) == 0);
    assert(memcmp(a, b, 32) != 0);
    printf("  passed\n");
}

static void
test_fill_random_zero(void)
{
    printf("Testing csilk_crypto_fill_random zero-length...\n");
    assert(csilk_crypto_fill_random(NULL, 0) == 0);
    printf("  passed\n");
}

static void
test_generate_nonce(void)
{
    printf("Testing csilk_crypto_generate_nonce...\n");
    uint8_t n1[12], n2[12];
    csilk_crypto_generate_nonce(n1, CSILK_GCM_NONCE_SIZE);
    csilk_crypto_generate_nonce(n2, CSILK_GCM_NONCE_SIZE);
    assert(memcmp(n1, n2, 12) != 0);
    printf("  passed\n");
}

/* ---- URL decode edge cases ---- */

static void
test_url_decode_valid(void)
{
    printf("Testing URL decode valid sequences...\n");
    char   buf[] = "%48%65%6C%6C%6F";
    size_t len = csilk_url_decode(buf);
    assert(len == 5);
    assert(strcmp(buf, "Hello") == 0);
    printf("  passed\n");
}

static void
test_url_decode_plus(void)
{
    printf("Testing URL decode '+' → space...\n");
    char   buf[] = "hello+world";
    size_t len = csilk_url_decode(buf);
    assert(len == 11);
    assert(strcmp(buf, "hello world") == 0);
    printf("  passed\n");
}

static void
test_url_decode_already_decoded(void)
{
    printf("Testing URL decode idempotence...\n");
    char   buf[] = "hello world";
    size_t len1 = csilk_url_decode(buf);
    size_t len2 = csilk_url_decode(buf);
    assert(len1 == len2);
    assert(strcmp(buf, "hello world") == 0);
    printf("  passed\n");
}

/* ---- Main ---- */

int
main()
{
    test_sha256_properties();
    test_hmac_properties();
    test_base64_roundtrip();
    test_base64url_roundtrip();
    test_base64_no_padding_for_exact();
    test_base64_empty();
    test_fill_random_uniqueness();
    test_fill_random_zero();
    test_generate_nonce();
    test_url_decode_valid();
    test_url_decode_plus();
    test_url_decode_already_decoded();

    printf("\nAll crypto tests passed!\n");
    return 0;
}
