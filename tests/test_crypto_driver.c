/**
 * @file test_crypto_driver.c
 * @brief Tests for pluggable Crypto Driver interface.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static int custom_uuid_called = 0;
static int custom_hmac_called = 0;

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

static csilk_crypto_driver_t my_driver = {
    .generate_uuid = custom_generate_uuid,
    .hmac_sha256 = custom_hmac_sha256,
    .sha256 = NULL // Use default
};

int
main()
{
	printf("Testing Crypto Driver interface...\n");

	csilk_ctx_t c;
	memset(&c, 0, sizeof(c));

	// Default behavior
	char uuid[37];
	_csilk_generate_uuid(&c, uuid);
	assert(custom_uuid_called == 0);
	assert(strlen(uuid) == 36);

	// Plug in driver
	c.crypto_driver = &my_driver;

	_csilk_generate_uuid(&c, uuid);
	assert(custom_uuid_called == 1);
	assert(strcmp(uuid, "custom-uuid-1234-5678-90abcdef1234") == 0);

	uint8_t sig[32];
	_csilk_hmac_sha256(&c, (uint8_t*)"key", 3, (uint8_t*)"data", 4, sig);
	assert(custom_hmac_called == 1);
	assert(sig[0] == 0x42);

	printf("Crypto Driver interface tests passed!\n");
	return 0;
}
