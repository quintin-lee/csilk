#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

static void
test_sha256_basic()
{
	printf("Testing csilk_sha256 basic...\n");
	csilk_sha256_ctx ctx;
	uint8_t digest[32];

	csilk_sha256_init(&ctx);
	csilk_sha256_update(&ctx, (uint8_t*)"abc", 3);
	csilk_sha256_final(&ctx, digest);

	uint8_t expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
			      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
			      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	assert(memcmp(digest, expected, 32) == 0);
	printf("csilk_sha256 basic passed!\n");
}

static void
test_sha256_empty()
{
	printf("Testing csilk_sha256 empty...\n");
	csilk_sha256_ctx ctx;
	uint8_t digest[32];

	csilk_sha256_init(&ctx);
	csilk_sha256_final(&ctx, digest);

	uint8_t expected[] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
			      0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
			      0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
	assert(memcmp(digest, expected, 32) == 0);
	printf("csilk_sha256 empty passed!\n");
}

static void
test_sha256_multi_update()
{
	printf("Testing csilk_sha256 multi-update...\n");
	csilk_sha256_ctx ctx;
	uint8_t digest[32];

	csilk_sha256_init(&ctx);
	csilk_sha256_update(&ctx, (uint8_t*)"a", 1);
	csilk_sha256_update(&ctx, (uint8_t*)"b", 1);
	csilk_sha256_update(&ctx, (uint8_t*)"c", 1);
	csilk_sha256_final(&ctx, digest);

	uint8_t expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
			      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
			      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	assert(memcmp(digest, expected, 32) == 0);
	printf("csilk_sha256 multi-update passed!\n");
}

static void
test_sha256_large()
{
	printf("Testing csilk_sha256 large input (>64 bytes)...\n");
	csilk_sha256_ctx ctx;
	uint8_t digest[32];
	char big[200];
	memset(big, 'A', 200);

	csilk_sha256_init(&ctx);
	csilk_sha256_update(&ctx, (uint8_t*)big, 200);
	csilk_sha256_final(&ctx, digest);

	uint8_t expected[] = {0x70, 0xd3, 0xbf, 0x8b, 0x0b, 0x9d, 0x83, 0xa6, 0x10, 0x12, 0xf3,
			      0x5f, 0xbf, 0x46, 0x0c, 0x42, 0x07, 0x06, 0x3f, 0xe3, 0x1b, 0x4d,
			      0x61, 0x78, 0x39, 0x0f, 0xe3, 0xb7, 0x21, 0xcc, 0x03, 0xf7};
	assert(memcmp(digest, expected, 32) == 0);
	printf("csilk_sha256 large passed!\n");
}

static void
test_hmac_sha256_rfc4231()
{
	printf("Testing HMAC-SHA256 RFC 4231 test case 2...\n");
	uint8_t out[32];
	const char* key = "Jefe";
	const char* data = "what do ya want for nothing?";

	csilk_hmac_sha256((uint8_t*)key, strlen(key), (uint8_t*)data, strlen(data), out);

	uint8_t expected[] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
			      0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
			      0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
	assert(memcmp(out, expected, 32) == 0);
	printf("HMAC-SHA256 RFC 4231 passed!\n");
}

static void
test_hmac_sha256_long_key()
{
	printf("Testing HMAC-SHA256 with key > 64 bytes...\n");
	uint8_t out[32];
	char long_key[100];
	memset(long_key, 'K', 100);

	csilk_hmac_sha256((uint8_t*)long_key, 100, (uint8_t*)"test data", 9, out);
	assert(out[0] != 0);
	printf("HMAC-SHA256 long key passed!\n");
}

static void
test_base64url_encode()
{
	printf("Testing csilk_base64url_encode...\n");
	char out[64];

	csilk_base64url_encode((uint8_t*)"", 0, out);
	assert(strcmp(out, "") == 0);

	csilk_base64url_encode((uint8_t*)"f", 1, out);
	assert(strcmp(out, "Zg") == 0);

	csilk_base64url_encode((uint8_t*)"fo", 2, out);
	assert(strcmp(out, "Zm8") == 0);

	csilk_base64url_encode((uint8_t*)"foo", 3, out);
	assert(strcmp(out, "Zm9v") == 0);

	printf("csilk_base64url_encode passed!\n");
}

static void
test_base64url_encode_binary()
{
	printf("Testing csilk_base64url_encode binary...\n");
	char out[64];
	uint8_t raw[] = {0x14, 0xFB, 0x9C, 0x03, 0xD9, 0x7E};
	csilk_base64url_encode(raw, 6, out);
	assert(strcmp(out, "FPucA9l-") == 0);
	printf("csilk_base64url_encode binary passed!\n");
}

static void
test_base64url_decode()
{
	printf("Testing csilk_base64url_decode...\n");
	uint8_t out[64];

	int len = csilk_base64url_decode("", out);
	assert(len == 0);

	len = csilk_base64url_decode("Zg", out);
	assert(len == 1 && out[0] == 'f');

	len = csilk_base64url_decode("Zm8", out);
	assert(len == 2 && memcmp(out, "fo", 2) == 0);

	len = csilk_base64url_decode("Zm9v", out);
	assert(len == 3 && memcmp(out, "foo", 3) == 0);

	len = csilk_base64url_decode("Zm9vYmE", out);
	assert(len == 5 && memcmp(out, "fooba", 5) == 0);

	len = csilk_base64url_decode("Zm9vYmFy", out);
	assert(len == 6 && memcmp(out, "foobar", 6) == 0);

	printf("csilk_base64url_decode passed!\n");
}

static void
test_base64url_decode_urlsafe()
{
	printf("Testing csilk_base64url_decode URL-safe chars...\n");
	uint8_t out[64];
	int len = csilk_base64url_decode("FPucA9l-", out);
	assert(len == 6);
	uint8_t expected[] = {0x14, 0xFB, 0x9C, 0x03, 0xD9, 0x7E};
	assert(memcmp(out, expected, 6) == 0);
	printf("csilk_base64url_decode URL-safe passed!\n");
}

static void
test_base64url_decode_invalid()
{
	printf("Testing csilk_base64url_decode invalid input...\n");
	uint8_t out[64];
	int len = csilk_base64url_decode("!!!invalid!!!", out);
	assert(len == -1);
	printf("csilk_base64url_decode invalid passed!\n");
}

static void
test_generate_uuid()
{
	printf("Testing csilk_generate_uuid...\n");
	char buf[37] = {0};
	csilk_generate_uuid(buf);
	assert(strlen(buf) == 36);
	assert(buf[8] == '-');
	assert(buf[13] == '-');
	assert(buf[18] == '-');
	assert(buf[23] == '-');
	assert(buf[14] == '4');
	printf("csilk_generate_uuid passed! uuid=%s\n", buf);
}

static void
test_csilk_hmac_sha256()
{
	printf("Testing _csilk_hmac_sha256...\n");
	uint8_t out[32];
	_csilk_hmac_sha256(nullptr, (uint8_t*)"key", 3, (uint8_t*)"data", 4, out);

	uint8_t expected[32];
	csilk_hmac_sha256((uint8_t*)"key", 3, (uint8_t*)"data", 4, expected);
	assert(memcmp(out, expected, 32) == 0);
	printf("_csilk_hmac_sha256 passed!\n");
}

static int hmac_driver_called = 0;
static void
test_hmac_driver_cb(
    const uint8_t* key, size_t klen, const uint8_t* data, size_t dlen, uint8_t* result)
{
	(void)key;
	(void)klen;
	(void)data;
	(void)dlen;
	hmac_driver_called = 1;
	memset(result, 0xAB, 32);
}

static void
test_csilk_hmac_sha256_with_crypto_driver()
{
	printf("Testing _csilk_hmac_sha256 with crypto driver...\n");
	uint8_t out[32] = {0};
	hmac_driver_called = 0;

	csilk_crypto_driver_t driver;
	memset(&driver, 0, sizeof(driver));
	driver.hmac_sha256 = test_hmac_driver_cb;

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_ctx_set_crypto_driver(ctx, &driver);
	_csilk_hmac_sha256(ctx, (uint8_t*)"k", 1, (uint8_t*)"d", 1, out);
	assert(hmac_driver_called == 1);
	assert(out[0] == 0xAB);
	csilk_test_ctx_free(ctx);
	printf("_csilk_hmac_sha256 with crypto driver passed!\n");
}

static int uuid_driver_called = 0;
static void
test_uuid_driver_cb(char* buf)
{
	uuid_driver_called = 1;
	strcpy(buf, "driver-uuid-test-1234567890");
}

static void
test_csilk_generate_uuid_with_driver()
{
	printf("Testing _csilk_generate_uuid with crypto driver...\n");
	uuid_driver_called = 0;

	csilk_crypto_driver_t driver;
	memset(&driver, 0, sizeof(driver));
	driver.generate_uuid = test_uuid_driver_cb;

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_ctx_set_crypto_driver(ctx, &driver);

	char buf[37] = {0};
	_csilk_generate_uuid(ctx, buf);
	assert(uuid_driver_called == 1);
	assert(strcmp(buf, "driver-uuid-test-1234567890") == 0);

	uuid_driver_called = 0;
	_csilk_generate_uuid(nullptr, buf);
	assert(strcmp(buf, "driver-uuid-test-1234567890") != 0);
	csilk_test_ctx_free(ctx);
	printf("_csilk_generate_uuid with crypto driver passed!\n");
}

int
main()
{
	test_sha256_basic();
	test_sha256_empty();
	test_sha256_multi_update();
	test_sha256_large();
	test_hmac_sha256_rfc4231();
	test_hmac_sha256_long_key();
	test_base64url_encode();
	test_base64url_encode_binary();
	test_base64url_decode();
	test_base64url_decode_urlsafe();
	test_base64url_decode_invalid();
	test_generate_uuid();
	test_csilk_hmac_sha256();
	test_csilk_hmac_sha256_with_crypto_driver();
	test_csilk_generate_uuid_with_driver();
	printf("test_utils_ext: ALL PASSED\n");
	return 0;
}
