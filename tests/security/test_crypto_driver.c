/**
 * @file test_crypto_driver.c
 * @brief Tests for pluggable Crypto Driver interface.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/test/test.h"

static int custom_uuid_called = 0;
static int custom_hmac_called = 0;
static int custom_sha1_called = 0;
static int custom_bcrypt_called = 0;

static void
custom_generate_uuid(char buf[37])
{
    strcpy(buf, "custom-uuid-1234-5678-90abcdef1234");
    custom_uuid_called++;
}

static void
custom_hmac_sha256(
    const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32])
{
    (void)key;
    (void)key_len;
    (void)data;
    (void)data_len;
    memset(out, 0x42, 32);
    custom_hmac_called++;
}

static void
custom_sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    (void)len;
    memset(out, 0x99, 20);
    custom_sha1_called++;
    (void)data;
}

static void
custom_bcrypt_hash(const char* password, size_t len, int cost, char hash[62])
{
    (void)password;
    (void)len;
    (void)cost;
    strcpy(hash, "$2a$12$AAAAAAAAAAAAAAAAAAAAAObXZmxk5VQA");
    custom_bcrypt_called++;
}

static csilk_crypto_driver_t my_driver = {.generate_uuid = custom_generate_uuid,
                                          .hmac_sha256 = custom_hmac_sha256,
                                          .sha1 = custom_sha1,
                                          .bcrypt_hash = custom_bcrypt_hash,
                                          .sha256 = nullptr};

int
main()
{
    printf("Testing Crypto Driver interface...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    // Default behavior
    char uuid[37];
    _csilk_generate_uuid(c, uuid);
    assert(custom_uuid_called == 0);
    assert(strlen(uuid) == 36);

    // Plug in driver
    csilk_ctx_set_crypto_driver(c, &my_driver);

    _csilk_generate_uuid(c, uuid);
    assert(custom_uuid_called == 1);
    assert(strcmp(uuid, "custom-uuid-1234-5678-90abcdef1234") == 0);

    uint8_t sig[32];
    _csilk_hmac_sha256(c, (uint8_t*)"key", 3, (uint8_t*)"data", 4, sig);
    assert(custom_hmac_called == 1);
    assert(sig[0] == 0x42);

    // SHA-1 via driver
    uint8_t sha1_out[20];
    _csilk_sha1(c, (uint8_t*)"hello", 5, sha1_out);
    assert(custom_sha1_called == 1);
    assert(sha1_out[0] == 0x99);

    // bcrypt via driver
    char hash[62];
    _csilk_bcrypt_hash(c, "test", 4, 12, hash);
    assert(custom_bcrypt_called == 1);
    assert(strcmp(hash, "$2a$12$AAAAAAAAAAAAAAAAAAAAAObXZmxk5VQA") == 0);

    csilk_test_ctx_free(c);
    printf("Crypto Driver interface tests passed!\n");
    return 0;
}
