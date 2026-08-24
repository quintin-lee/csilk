#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/drivers/cipher.h"
#include "csilk/core/cipher.h"
#include "csilk/test/test.h"

static int custom_encrypt_called = 0;
static int custom_decrypt_called = 0;
static int custom_keygen_called = 0;

static int
my_symmetric_encrypt(const uint8_t* key,
                     size_t         key_len,
                     const uint8_t* plaintext,
                     size_t         plaintext_len,
                     const uint8_t* iv,
                     size_t         iv_len,
                     uint8_t*       ciphertext,
                     size_t*        ciphertext_len,
                     uint8_t*       tag,
                     size_t         tag_len)
{
    (void)key;
    (void)key_len;
    (void)iv;
    (void)iv_len;
    (void)tag;
    (void)tag_len;
    memcpy(ciphertext, plaintext, plaintext_len);
    *ciphertext_len = plaintext_len;
    custom_encrypt_called++;
    return 0;
}

static int
my_symmetric_decrypt(const uint8_t* key,
                     size_t         key_len,
                     const uint8_t* ciphertext,
                     size_t         ciphertext_len,
                     const uint8_t* iv,
                     size_t         iv_len,
                     const uint8_t* tag,
                     size_t         tag_len,
                     uint8_t*       plaintext,
                     size_t*        plaintext_len)
{
    (void)key;
    (void)key_len;
    (void)iv;
    (void)iv_len;
    (void)tag;
    (void)tag_len;
    memcpy(plaintext, ciphertext, ciphertext_len);
    *plaintext_len = ciphertext_len;
    custom_decrypt_called++;
    return 0;
}

static int
my_generate_keypair(char* public_key, size_t* pub_len, char* private_key, size_t* priv_len)
{
    const char* pub = "custom-public-key";
    const char* priv = "custom-private-key";
    size_t      pl = strlen(pub) + 1;
    size_t      prl = strlen(priv) + 1;
    if (pl > *pub_len || prl > *priv_len) {
        *pub_len = pl;
        *priv_len = prl;
        return -1;
    }
    memcpy(public_key, pub, pl);
    *pub_len = pl;
    memcpy(private_key, priv, prl);
    *priv_len = prl;
    custom_keygen_called++;
    return 0;
}

static csilk_cipher_driver_t my_driver = {
    .symmetric_encrypt = my_symmetric_encrypt,
    .symmetric_decrypt = my_symmetric_decrypt,
    .generate_keypair = my_generate_keypair,
    .asymmetric_encrypt = nullptr,
    .asymmetric_decrypt = nullptr,
    .sign = nullptr,
    .verify = nullptr,
};

static void
test_default_symmetric_roundtrip(void)
{
    printf("  Testing default symmetric encrypt/decrypt...\n");

    uint8_t key[32];
    uint8_t iv[12];
    memset(key, 0x2A, 32);
    memset(iv, 0x3B, 12);

    const uint8_t plaintext[] = "Hello, AES-256-GCM! This is a secret message.";
    size_t        pt_len = strlen((const char*)plaintext);

    uint8_t ciphertext[256];
    size_t  ct_len = sizeof(ciphertext);
    uint8_t tag[16];

    int r = _csilk_symmetric_encrypt(nullptr,
                                     key,
                                     sizeof(key),
                                     plaintext,
                                     pt_len,
                                     iv,
                                     sizeof(iv),
                                     ciphertext,
                                     &ct_len,
                                     tag,
                                     sizeof(tag));
    assert(r == 0);
    assert(ct_len == pt_len);

    uint8_t decrypted[256];
    size_t  dec_len = sizeof(decrypted);
    r = _csilk_symmetric_decrypt(nullptr,
                                 key,
                                 sizeof(key),
                                 ciphertext,
                                 ct_len,
                                 iv,
                                 sizeof(iv),
                                 tag,
                                 sizeof(tag),
                                 decrypted,
                                 &dec_len);
    assert(r == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    printf("    Symmetric roundtrip OK\n");
}

static void
test_default_symmetric_wrong_tag(void)
{
    printf("  Testing default symmetric decrypt with wrong tag...\n");

    uint8_t key[32];
    uint8_t iv[12];
    memset(key, 0x2A, 32);
    memset(iv, 0x3B, 12);

    const uint8_t plaintext[] = "Test message";
    size_t        pt_len = strlen((const char*)plaintext);

    uint8_t ciphertext[256];
    size_t  ct_len = sizeof(ciphertext);
    uint8_t tag[16];

    int r = _csilk_symmetric_encrypt(nullptr,
                                     key,
                                     sizeof(key),
                                     plaintext,
                                     pt_len,
                                     iv,
                                     sizeof(iv),
                                     ciphertext,
                                     &ct_len,
                                     tag,
                                     sizeof(tag));
    assert(r == 0);

    tag[0] ^= 0xFF;

    uint8_t decrypted[256];
    size_t  dec_len = sizeof(decrypted);
    r = _csilk_symmetric_decrypt(nullptr,
                                 key,
                                 sizeof(key),
                                 ciphertext,
                                 ct_len,
                                 iv,
                                 sizeof(iv),
                                 tag,
                                 sizeof(tag),
                                 decrypted,
                                 &dec_len);
    assert(r == -1);

    printf("    Wrong tag correctly rejected\n");
}

static void
test_default_symmetric_bad_key(void)
{
    printf("  Testing default symmetric with wrong key size...\n");

    uint8_t key[16];
    uint8_t iv[12];
    uint8_t ciphertext[256];
    size_t  ct_len = sizeof(ciphertext);
    uint8_t tag[16];

    int r = _csilk_symmetric_encrypt(nullptr,
                                     key,
                                     sizeof(key),
                                     (const uint8_t*)"data",
                                     4,
                                     iv,
                                     sizeof(iv),
                                     ciphertext,
                                     &ct_len,
                                     tag,
                                     sizeof(tag));
    assert(r == -1);

    printf("    Bad key size correctly rejected\n");
}

static void
test_default_asymmetric_roundtrip(void)
{
    printf("  Testing default keygen + asymmetric encrypt/decrypt...\n");

    char   pub_key[2048];
    char   priv_key[4096];
    size_t pub_len = sizeof(pub_key);
    size_t priv_len = sizeof(priv_key);

    int r = _csilk_generate_keypair(nullptr, pub_key, &pub_len, priv_key, &priv_len);
    assert(r == 0);
    assert(pub_len > 0);
    assert(priv_len > 0);

    const uint8_t plaintext[] = "RSA-OAEP test data";
    size_t        pt_len = strlen((const char*)plaintext);

    uint8_t ciphertext[CSILK_RSA_KEY_SIZE];
    size_t  ct_len = sizeof(ciphertext);

    r = _csilk_asymmetric_encrypt(
        nullptr, pub_key, pub_len, plaintext, pt_len, ciphertext, &ct_len);
    assert(r == 0);
    assert(ct_len == CSILK_RSA_KEY_SIZE);

    uint8_t decrypted[256];
    size_t  dec_len = sizeof(decrypted);

    r = _csilk_asymmetric_decrypt(
        nullptr, priv_key, priv_len, ciphertext, ct_len, decrypted, &dec_len);
    assert(r == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    printf("    Asymmetric roundtrip OK\n");
}

static void
test_default_sign_verify(void)
{
    printf("  Testing default sign/verify...\n");

    char   pub_key[2048];
    char   priv_key[4096];
    size_t pub_len = sizeof(pub_key);
    size_t priv_len = sizeof(priv_key);

    int r = _csilk_generate_keypair(nullptr, pub_key, &pub_len, priv_key, &priv_len);
    assert(r == 0);

    const uint8_t data[] = "Data to sign with RSA-PSS";
    size_t        data_len = strlen((const char*)data);

    uint8_t sig[CSILK_RSA_SIGNATURE_SIZE];
    size_t  sig_len = sizeof(sig);

    r = _csilk_sign(nullptr, priv_key, priv_len, data, data_len, sig, &sig_len);
    assert(r == 0);
    assert(sig_len == CSILK_RSA_SIGNATURE_SIZE);

    r = _csilk_verify(nullptr, pub_key, pub_len, data, data_len, sig, sig_len);
    assert(r == 0);

    sig[0] ^= 0xFF;
    r = _csilk_verify(nullptr, pub_key, pub_len, data, data_len, sig, sig_len);
    assert(r == -1);

    printf("    Sign/verify OK\n");
}

static void
test_custom_driver_pluggable(void)
{
    printf("  Testing custom cipher driver plugin...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_ctx_set_cipher_driver(c, &my_driver);

    uint8_t       key[32];
    uint8_t       iv[12];
    const uint8_t pt[] = "custom test";
    size_t        pt_len = strlen((const char*)pt);
    uint8_t       ct[64];
    size_t        ct_len = sizeof(ct);
    uint8_t       tag[16];

    custom_encrypt_called = 0;
    int r = _csilk_symmetric_encrypt(
        c, key, sizeof(key), pt, pt_len, iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));
    assert(r == 0);
    assert(custom_encrypt_called == 1);

    uint8_t dec[64];
    size_t  dec_len = sizeof(dec);
    custom_decrypt_called = 0;
    r = _csilk_symmetric_decrypt(
        c, key, sizeof(key), ct, ct_len, iv, sizeof(iv), tag, sizeof(tag), dec, &dec_len);
    assert(r == 0);
    assert(custom_decrypt_called == 1);
    assert(dec_len == pt_len);
    assert(memcmp(dec, pt, pt_len) == 0);

    csilk_test_ctx_free(c);
    printf("    Custom driver plugin OK\n");
}

static void
test_custom_keygen(void)
{
    printf("  Testing custom keygen...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_ctx_set_cipher_driver(c, &my_driver);

    char   pub[128];
    char   priv[128];
    size_t pub_len = sizeof(pub);
    size_t priv_len = sizeof(priv);

    custom_keygen_called = 0;
    int r = _csilk_generate_keypair(c, pub, &pub_len, priv, &priv_len);
    assert(r == 0);
    assert(custom_keygen_called == 1);
    assert(strcmp(pub, "custom-public-key") == 0);
    assert(strcmp(priv, "custom-private-key") == 0);

    csilk_test_ctx_free(c);
    printf("    Custom keygen OK\n");
}

static void
test_null_context_defaults(void)
{
    printf("  Testing nullptr context falls back to defaults...\n");

    uint8_t key[32];
    uint8_t iv[12];
    memset(key, 0xAA, 32);
    memset(iv, 0xBB, 12);

    const uint8_t pt[] = "null context test";
    size_t        pt_len = strlen((const char*)pt);
    uint8_t       ct[256];
    size_t        ct_len = sizeof(ct);
    uint8_t       tag[16];

    int r = _csilk_symmetric_encrypt(
        nullptr, key, sizeof(key), pt, pt_len, iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));
    assert(r == 0);

    uint8_t dec[256];
    size_t  dec_len = sizeof(dec);
    r = _csilk_symmetric_decrypt(
        nullptr, key, sizeof(key), ct, ct_len, iv, sizeof(iv), tag, sizeof(tag), dec, &dec_len);
    assert(r == 0);
    assert(dec_len == pt_len);
    assert(memcmp(dec, pt, pt_len) == 0);

    printf("    nullptr context defaults OK\n");
}

static void test_public_symmetric_roundtrip(void);
static void test_public_symmetric_bad_params(void);
static void test_public_rsa_roundtrip(void);
static void test_public_sign_verify(void);
static void test_public_null_params(void);

/* ============================================================================
 * Public API tests — csilk_symmetric_*, csilk_rsa_*
 * ============================================================================ */

static void
test_public_symmetric_roundtrip(void)
{
    printf("  Testing public symmetric encrypt/decrypt...\n");

    uint8_t key[CSILK_AES256_KEY_SIZE] = {0};
    for (int i = 0; i < CSILK_AES256_KEY_SIZE; i++) {
        key[i] = (uint8_t)i + 1;
    }

    uint8_t iv[CSILK_GCM_IV_SIZE] = {0};
    for (int i = 0; i < CSILK_GCM_IV_SIZE; i++) {
        iv[i] = (uint8_t)i + 100;
    }

    const char* plaintext = "Hello, csilk cipher API!";
    size_t      pt_len = strlen(plaintext);

    uint8_t ciphertext[256];
    size_t  ct_len = sizeof(ciphertext);
    uint8_t tag[CSILK_GCM_TAG_SIZE] = {0};

    int rc = csilk_symmetric_encrypt(key,
                                     sizeof(key),
                                     (const uint8_t*)plaintext,
                                     pt_len,
                                     iv,
                                     sizeof(iv),
                                     ciphertext,
                                     &ct_len,
                                     tag,
                                     sizeof(tag));
    assert(rc == 0);
    assert(ct_len == pt_len);

    uint8_t decrypted[256];
    size_t  dec_len = sizeof(decrypted);
    rc = csilk_symmetric_decrypt(key,
                                 sizeof(key),
                                 ciphertext,
                                 ct_len,
                                 iv,
                                 sizeof(iv),
                                 tag,
                                 sizeof(tag),
                                 decrypted,
                                 &dec_len);
    assert(rc == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    printf("    Public symmetric roundtrip OK\n");
}

static void
test_public_symmetric_bad_params(void)
{
    printf("  Testing public symmetric with bad params...\n");

    uint8_t key[32] = {0};
    uint8_t iv[12] = {0};
    uint8_t tag[16] = {0};
    uint8_t ct[64] = {0};
    size_t  ct_len = 64;

    /* Wrong key size */
    uint8_t bad_key[16] = {0};
    int     rc = csilk_symmetric_encrypt(bad_key,
                                         sizeof(bad_key),
                                         (const uint8_t*)"hello",
                                         5,
                                         iv,
                                         sizeof(iv),
                                         ct,
                                         &ct_len,
                                         tag,
                                         sizeof(tag));
    assert(rc == -1);

    /* Wrong IV size */
    uint8_t bad_iv[8] = {0};
    ct_len = 64;
    rc = csilk_symmetric_encrypt(key,
                                 sizeof(key),
                                 (const uint8_t*)"hello",
                                 5,
                                 bad_iv,
                                 sizeof(bad_iv),
                                 ct,
                                 &ct_len,
                                 tag,
                                 sizeof(tag));
    assert(rc == -1);

    /* NULL ciphertext_len */
    rc = csilk_symmetric_encrypt(
        key, sizeof(key), (const uint8_t*)"hello", 5, iv, sizeof(iv), ct, NULL, tag, sizeof(tag));
    assert(rc == -1);

    printf("    Bad params correctly rejected\n");
}

static void
test_public_rsa_roundtrip(void)
{
    printf("  Testing public RSA encrypt/decrypt...\n");

    char   pub_key[2048];
    char   priv_key[4096];
    size_t pub_len = sizeof(pub_key);
    size_t priv_len = sizeof(priv_key);

    int rc = csilk_rsa_generate_keypair(pub_key, &pub_len, priv_key, &priv_len);
    assert(rc == 0);
    assert(pub_len > 0 && priv_len > 0);

    const char* plaintext = "secret message";
    uint8_t     ciphertext[CSILK_RSA_KEY_SIZE] = {0};
    size_t      ct_len = sizeof(ciphertext);

    rc = csilk_rsa_encrypt(pub_key,
                           strlen(pub_key) + 1,
                           (const uint8_t*)plaintext,
                           strlen(plaintext),
                           ciphertext,
                           &ct_len);
    assert(rc == 0);
    assert(ct_len == CSILK_RSA_KEY_SIZE);

    uint8_t decrypted[256];
    size_t  dec_len = sizeof(decrypted);
    rc = csilk_rsa_decrypt(priv_key, strlen(priv_key) + 1, ciphertext, ct_len, decrypted, &dec_len);
    assert(rc == 0);
    assert(dec_len == strlen(plaintext));
    assert(memcmp(decrypted, plaintext, dec_len) == 0);

    printf("    Public RSA roundtrip OK\n");
}

static void
test_public_sign_verify(void)
{
    printf("  Testing public sign/verify...\n");

    char   pub_key[2048];
    char   priv_key[4096];
    size_t pub_len = sizeof(pub_key);
    size_t priv_len = sizeof(priv_key);

    int rc = csilk_rsa_generate_keypair(pub_key, &pub_len, priv_key, &priv_len);
    assert(rc == 0);

    const char* data = "data to sign";
    uint8_t     signature[CSILK_RSA_SIGNATURE_SIZE] = {0};
    size_t      sig_len = sizeof(signature);

    rc = csilk_rsa_sign(
        priv_key, strlen(priv_key) + 1, (const uint8_t*)data, strlen(data), signature, &sig_len);
    assert(rc == 0);
    assert(sig_len == CSILK_RSA_SIGNATURE_SIZE);

    rc = csilk_rsa_verify(
        pub_key, strlen(pub_key) + 1, (const uint8_t*)data, strlen(data), signature, sig_len);
    assert(rc == 0);

    /* Wrong data should fail */
    rc = csilk_rsa_verify(
        pub_key, strlen(pub_key) + 1, (const uint8_t*)"tampered", 8, signature, sig_len);
    assert(rc == -1);

    printf("    Public sign/verify OK\n");
}

static void
test_public_null_params(void)
{
    printf("  Testing public API null parameter rejection...\n");

    uint8_t key[32] = {0};
    uint8_t iv[12] = {0};
    uint8_t tag[16] = {0};
    uint8_t ct[64] = {0};
    size_t  ct_len = 64;

    assert(csilk_symmetric_encrypt(
               NULL, 32, (const uint8_t*)"x", 1, iv, 12, ct, &ct_len, tag, 16) == -1);
    assert(csilk_symmetric_decrypt(key, 32, ct, ct_len, iv, 12, tag, 16, ct, NULL) == -1);
    assert(csilk_rsa_generate_keypair(NULL, NULL, NULL, NULL) == -1);
    assert(csilk_rsa_encrypt(NULL, 0, (const uint8_t*)"x", 1, ct, &ct_len) == -1);
    assert(csilk_rsa_decrypt(NULL, 0, ct, ct_len, ct, &ct_len) == -1);
    assert(csilk_rsa_sign(NULL, 0, (const uint8_t*)"x", 1, ct, &ct_len) == -1);
    assert(csilk_rsa_verify(NULL, 0, (const uint8_t*)"x", 1, ct, ct_len) == -1);

    printf("    Null params correctly rejected\n");
}

int
main()
{
    printf("Testing Cipher Driver interface...\n");

    test_default_symmetric_roundtrip();
    test_default_symmetric_wrong_tag();
    test_default_symmetric_bad_key();
    test_default_asymmetric_roundtrip();
    test_default_sign_verify();
    test_custom_driver_pluggable();
    test_custom_keygen();
    test_null_context_defaults();
    test_public_symmetric_roundtrip();
    test_public_symmetric_bad_params();
    test_public_rsa_roundtrip();
    test_public_sign_verify();
    test_public_null_params();

    printf("All cipher tests passed!\n");
    return 0;
}
