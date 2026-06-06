#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/test/test.h"

static int custom_encrypt_called = 0;
static int custom_decrypt_called = 0;
static int custom_keygen_called = 0;

static int
my_symmetric_encrypt(const uint8_t* key,
		     size_t key_len,
		     const uint8_t* plaintext,
		     size_t plaintext_len,
		     const uint8_t* iv,
		     size_t iv_len,
		     uint8_t* ciphertext,
		     size_t* ciphertext_len,
		     uint8_t* tag,
		     size_t tag_len)
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
		     size_t key_len,
		     const uint8_t* ciphertext,
		     size_t ciphertext_len,
		     const uint8_t* iv,
		     size_t iv_len,
		     const uint8_t* tag,
		     size_t tag_len,
		     uint8_t* plaintext,
		     size_t* plaintext_len)
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
	size_t pl = strlen(pub) + 1;
	size_t prl = strlen(priv) + 1;
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
	size_t pt_len = strlen((const char*)plaintext);

	uint8_t ciphertext[256];
	size_t ct_len = sizeof(ciphertext);
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
	size_t dec_len = sizeof(decrypted);
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
	size_t pt_len = strlen((const char*)plaintext);

	uint8_t ciphertext[256];
	size_t ct_len = sizeof(ciphertext);
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
	size_t dec_len = sizeof(decrypted);
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
	size_t ct_len = sizeof(ciphertext);
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

	char pub_key[2048];
	char priv_key[4096];
	size_t pub_len = sizeof(pub_key);
	size_t priv_len = sizeof(priv_key);

	int r = _csilk_generate_keypair(nullptr, pub_key, &pub_len, priv_key, &priv_len);
	assert(r == 0);
	assert(pub_len > 0);
	assert(priv_len > 0);

	const uint8_t plaintext[] = "RSA-OAEP test data";
	size_t pt_len = strlen((const char*)plaintext);

	uint8_t ciphertext[CSILK_RSA_KEY_SIZE];
	size_t ct_len = sizeof(ciphertext);

	r = _csilk_asymmetric_encrypt(
	    nullptr, pub_key, pub_len, plaintext, pt_len, ciphertext, &ct_len);
	assert(r == 0);
	assert(ct_len == CSILK_RSA_KEY_SIZE);

	uint8_t decrypted[256];
	size_t dec_len = sizeof(decrypted);

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

	char pub_key[2048];
	char priv_key[4096];
	size_t pub_len = sizeof(pub_key);
	size_t priv_len = sizeof(priv_key);

	int r = _csilk_generate_keypair(nullptr, pub_key, &pub_len, priv_key, &priv_len);
	assert(r == 0);

	const uint8_t data[] = "Data to sign with RSA-PSS";
	size_t data_len = strlen((const char*)data);

	uint8_t sig[CSILK_RSA_SIGNATURE_SIZE];
	size_t sig_len = sizeof(sig);

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

	uint8_t key[32];
	uint8_t iv[12];
	const uint8_t pt[] = "custom test";
	size_t pt_len = strlen((const char*)pt);
	uint8_t ct[64];
	size_t ct_len = sizeof(ct);
	uint8_t tag[16];

	custom_encrypt_called = 0;
	int r = _csilk_symmetric_encrypt(
	    c, key, sizeof(key), pt, pt_len, iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));
	assert(r == 0);
	assert(custom_encrypt_called == 1);

	uint8_t dec[64];
	size_t dec_len = sizeof(dec);
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

	char pub[128];
	char priv[128];
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
	size_t pt_len = strlen((const char*)pt);
	uint8_t ct[256];
	size_t ct_len = sizeof(ct);
	uint8_t tag[16];

	int r = _csilk_symmetric_encrypt(
	    nullptr, key, sizeof(key), pt, pt_len, iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));
	assert(r == 0);

	uint8_t dec[256];
	size_t dec_len = sizeof(dec);
	r = _csilk_symmetric_decrypt(
	    nullptr, key, sizeof(key), ct, ct_len, iv, sizeof(iv), tag, sizeof(tag), dec, &dec_len);
	assert(r == 0);
	assert(dec_len == pt_len);
	assert(memcmp(dec, pt, pt_len) == 0);

	printf("    nullptr context defaults OK\n");
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

	printf("All cipher tests passed!\n");
	return 0;
}
