#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"

static void
hex_dump(const uint8_t* data, size_t len, char* out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

static void
test_sha1_empty(void)
{
    printf("Testing SHA-1 (empty string)...\n");
    csilk_sha1_ctx ctx;
    uint8_t        digest[20];
    char           hex[41];

    csilk_sha1_init(&ctx);
    csilk_sha1_update(&ctx, (const uint8_t*)"", 0);
    csilk_sha1_final(&ctx, digest);
    hex_dump(digest, 20, hex);

    assert(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
    printf("  passed\n");
}

static void
test_sha1_abc(void)
{
    printf("Testing SHA-1 (\"abc\")...\n");
    csilk_sha1_ctx ctx;
    uint8_t        digest[20];
    char           hex[41];

    csilk_sha1_init(&ctx);
    csilk_sha1_update(&ctx, (const uint8_t*)"abc", 3);
    csilk_sha1_final(&ctx, digest);
    hex_dump(digest, 20, hex);

    assert(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    printf("  passed\n");
}

static void
test_sha1_multi_update(void)
{
    printf("Testing SHA-1 (multi-update \"abcd\" + \"efgh\")...\n");
    csilk_sha1_ctx ctx;
    uint8_t        digest[20];
    char           hex[41];

    csilk_sha1_init(&ctx);
    csilk_sha1_update(&ctx, (const uint8_t*)"abcd", 4);
    csilk_sha1_update(&ctx, (const uint8_t*)"efgh", 4);
    csilk_sha1_final(&ctx, digest);
    hex_dump(digest, 20, hex);

    csilk_sha1_ctx ctx2;
    uint8_t        digest2[20];
    char           hex2[41];
    csilk_sha1_init(&ctx2);
    csilk_sha1_update(&ctx2, (const uint8_t*)"abcdefgh", 8);
    csilk_sha1_final(&ctx2, digest2);
    hex_dump(digest2, 20, hex2);

    assert(strcmp(hex, hex2) == 0);
    printf("  passed\n");
}

static void
test_sha256_empty(void)
{
    printf("Testing SHA-256 (empty string)...\n");
    csilk_sha256_ctx ctx;
    uint8_t          digest[32];
    char             hex[65];

    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, (const uint8_t*)"", 0);
    csilk_sha256_final(&ctx, digest);
    hex_dump(digest, 32, hex);

    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    printf("  passed\n");
}

static void
test_sha256_abc(void)
{
    printf("Testing SHA-256 (\"abc\")...\n");
    csilk_sha256_ctx ctx;
    uint8_t          digest[32];
    char             hex[65];

    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, (const uint8_t*)"abc", 3);
    csilk_sha256_final(&ctx, digest);
    hex_dump(digest, 32, hex);

    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    printf("  passed\n");
}

static void
test_sha256_long(void)
{
    printf("Testing SHA-256 (\"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\")...\n");
    csilk_sha256_ctx ctx;
    uint8_t          digest[32];
    char             hex[65];

    const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, (const uint8_t*)msg, strlen(msg));
    csilk_sha256_final(&ctx, digest);
    hex_dump(digest, 32, hex);

    assert(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    printf("  passed\n");
}

static void
test_hmac_sha256_rfc4231_case1(void)
{
    printf("Testing HMAC-SHA256 (RFC 4231 Case 1)...\n");
    uint8_t key[20];
    memset(key, 0x0b, 20);
    const char* data = "Hi There";
    uint8_t     out[32];
    char        hex[65];

    csilk_hmac_sha256(key, 20, (const uint8_t*)data, 8, out);
    hex_dump(out, 32, hex);

    assert(strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);
    printf("  passed\n");
}

static void
test_hmac_sha256_rfc4231_case2(void)
{
    printf("Testing HMAC-SHA256 (RFC 4231 Case 2 - \"Jefe\")...\n");
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    uint8_t     out[32];
    char        hex[65];

    csilk_hmac_sha256((const uint8_t*)key, 4, (const uint8_t*)data, 28, out);
    hex_dump(out, 32, hex);

    assert(strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
    printf("  passed\n");
}

static void
test_base64_rfc4648(void)
{
    printf("Testing Base64 (RFC 4648 vectors)...\n");

    struct {
        const char* input;
        const char* expected;
    } vectors[] = {
        {"",       ""        },
        {"f",      "Zg=="    },
        {"fo",     "Zm8="    },
        {"foo",    "Zm9v"    },
        {"foob",   "Zm9vYg=="},
        {"fooba",  "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char out[64];
        csilk_base64_encode((const uint8_t*)vectors[i].input, strlen(vectors[i].input), out);
        assert(strcmp(out, vectors[i].expected) == 0);
    }

    printf("  passed\n");
}

static void
test_base64url_roundtrip(void)
{
    printf("Testing Base64URL round-trip...\n");

    const uint8_t data[] = "\xff\xfe\xfd\xfc\xfb\x00\x01\x02";
    size_t        len = 8;
    char          encoded[64];
    uint8_t       decoded[64];

    csilk_base64url_encode(data, len, encoded);

    for (size_t i = 0; encoded[i]; i++) {
        assert(encoded[i] != '+' && encoded[i] != '/' && encoded[i] != '=');
    }

    int dec_len = csilk_base64url_decode(encoded, decoded, sizeof(decoded));
    assert(dec_len == (int)len);
    assert(memcmp(decoded, data, len) == 0);

    printf("  passed\n");
}

static void
test_base64url_decode_invalid(void)
{
    printf("Testing Base64URL decode invalid input...\n");

    uint8_t out[64];
    int     r = csilk_base64url_decode("!!!invalid!!!", out, sizeof(out));
    assert(r == -1);

    printf("  passed\n");
}

static void
test_base64_binary_data(void)
{
    printf("Testing Base64 binary data round-trip...\n");

    uint8_t binary[256];
    for (int i = 0; i < 256; i++) {
        binary[i] = (uint8_t)i;
    }

    char encoded[512];
    csilk_base64_encode(binary, 256, encoded);

    uint8_t decoded[256];
    int     dec_len = csilk_base64url_decode(encoded, decoded, sizeof(decoded));
    assert(dec_len == 256);
    assert(memcmp(decoded, binary, 256) == 0);

    printf("  passed\n");
}

int
main(void)
{
    test_sha1_empty();
    test_sha1_abc();
    test_sha1_multi_update();
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_long();
    test_hmac_sha256_rfc4231_case1();
    test_hmac_sha256_rfc4231_case2();
    test_base64_rfc4648();
    test_base64url_roundtrip();
    test_base64url_decode_invalid();
    test_base64_binary_data();

    printf("test_crypto_primitives: ALL PASSED\n");
    return 0;
}
