#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/crypto_dispatch.h"
#include "csilk/core/hash.h"

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

static void
test_sha256_deterministic(void)
{
    printf("Testing SHA-256 determinism...\n");
    uint64_t seed = 101;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t data[128];
        size_t  len = xorshift64(&seed) % 128;
        for (size_t j = 0; j < len; j++) {
            data[j] = rand_byte(&seed);
        }

        uint8_t          h1[32];
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, len);
        csilk_sha256_final(&ctx, h1);

        uint8_t h2[32];
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, len);
        csilk_sha256_final(&ctx, h2);

        assert(memcmp(h1, h2, 32) == 0);
    }
    printf("test_sha256_deterministic passed\n");
}

static void
test_sha256_multi_update(void)
{
    printf("Testing SHA-256 multi-update equivalence...\n");
    uint64_t seed = 202;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t data[256];
        size_t  len = 1 + (xorshift64(&seed) % 200);
        for (size_t j = 0; j < len; j++) {
            data[j] = rand_byte(&seed);
        }

        uint8_t          single_hash[32];
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, len);
        csilk_sha256_final(&ctx, single_hash);

        uint8_t multi_hash[32];
        csilk_sha256_init(&ctx);
        size_t offset = 0;
        while (offset < len) {
            size_t chunk = xorshift64(&seed) % 17 + 1;
            if (chunk > len - offset) {
                chunk = len - offset;
            }
            csilk_sha256_update(&ctx, data + offset, chunk);
            offset += chunk;
        }
        csilk_sha256_final(&ctx, multi_hash);

        assert(memcmp(single_hash, multi_hash, 32) == 0);
    }
    printf("test_sha256_multi_update passed\n");
}

static void
test_sha256_length(void)
{
    printf("Testing SHA-256 output length...\n");
    uint64_t seed = 303;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t data[256];
        size_t  len = xorshift64(&seed) % 256;
        for (size_t j = 0; j < len; j++) {
            data[j] = rand_byte(&seed);
        }

        uint8_t          hash[32];
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data, len);
        csilk_sha256_final(&ctx, hash);

        uint8_t zero[32] = {0};
        assert(memcmp(hash, zero, 32) != 0 || len == 0);
    }
    printf("test_sha256_length passed\n");
}

static void
test_sha256_differs(void)
{
    printf("Testing SHA-256 different inputs differ...\n");
    uint64_t seed = 404;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t data_a[64];
        uint8_t data_b[64];
        size_t  len_a = 1 + (xorshift64(&seed) % 63);
        size_t  len_b = 1 + (xorshift64(&seed) % 63);
        for (size_t j = 0; j < len_a; j++) {
            data_a[j] = rand_byte(&seed);
        }
        for (size_t j = 0; j < len_b; j++) {
            data_b[j] = rand_byte(&seed);
        }

        uint8_t          ha[32], hb[32];
        csilk_sha256_ctx ctx;
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data_a, len_a);
        csilk_sha256_final(&ctx, ha);

        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, data_b, len_b);
        csilk_sha256_final(&ctx, hb);

        if (len_a != len_b || memcmp(data_a, data_b, len_a) != 0) {
            assert(memcmp(ha, hb, 32) != 0);
        }
    }
    printf("test_sha256_differs passed\n");
}

static void
test_hmac_deterministic(void)
{
    printf("Testing HMAC-SHA256 determinism...\n");
    uint64_t seed = 505;
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t key[32];
        uint8_t data[128];
        size_t  klen = 1 + (xorshift64(&seed) % 31);
        size_t  dlen = 1 + (xorshift64(&seed) % 127);
        for (size_t j = 0; j < klen; j++) {
            key[j] = rand_byte(&seed);
        }
        for (size_t j = 0; j < dlen; j++) {
            data[j] = rand_byte(&seed);
        }

        uint8_t h1[32], h2[32];
        csilk_hmac_sha256(key, klen, data, dlen, h1);
        csilk_hmac_sha256(key, klen, data, dlen, h2);
        assert(memcmp(h1, h2, 32) == 0);
    }
    printf("test_hmac_deterministic passed\n");
}

static void
test_hmac_different_keys(void)
{
    printf("Testing HMAC-SHA256 different keys differ...\n");
    uint64_t seed = 606;
    uint8_t  data[64];
    size_t   dlen = 32;
    for (size_t j = 0; j < dlen; j++) {
        data[j] = rand_byte(&seed);
    }

    for (int i = 0; i < PROP_ITERATIONS; i++) {
        uint8_t key_a[32], key_b[32];
        size_t  klen = 1 + (xorshift64(&seed) % 31);
        for (size_t j = 0; j < klen; j++) {
            key_a[j] = rand_byte(&seed);
            key_b[j] = rand_byte(&seed);
        }

        uint8_t ha[32], hb[32];
        csilk_hmac_sha256(key_a, klen, data, dlen, ha);
        csilk_hmac_sha256(key_b, klen, data, dlen, hb);
        assert(memcmp(ha, hb, 32) != 0);
    }
    printf("test_hmac_different_keys passed\n");
}

static void
test_uuid_format(void)
{
    printf("Testing UUID format...\n");
    for (int i = 0; i < PROP_ITERATIONS; i++) {
        char buf[37] = {0};
        csilk_generate_uuid(buf);
        assert(strlen(buf) == 36);
        assert(buf[8] == '-');
        assert(buf[13] == '-');
        assert(buf[18] == '-');
        assert(buf[23] == '-');
        assert(buf[14] == '4');
        for (int j = 0; j < 36; j++) {
            if (j == 8 || j == 13 || j == 18 || j == 23) {
                continue;
            }
            char c = buf[j];
            int  ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            assert(ok);
        }
    }
    printf("test_uuid_format passed\n");
}

static void
test_uuid_unique(void)
{
    printf("Testing UUID uniqueness...\n");
    char uuids[100][37];
    for (int i = 0; i < 100; i++) {
        csilk_generate_uuid(uuids[i]);
        assert(strlen(uuids[i]) == 36);
        for (int j = 0; j < i; j++) {
            assert(strcmp(uuids[i], uuids[j]) != 0);
        }
    }
    printf("test_uuid_unique passed\n");
}

int
main(void)
{
    test_sha256_deterministic();
    test_sha256_multi_update();
    test_sha256_length();
    test_sha256_differs();
    test_hmac_deterministic();
    test_hmac_different_keys();
    test_uuid_format();
    test_uuid_unique();
    printf("test_hash_prop: ALL PASSED\n");
    return 0;
}
