/**
 * @file uuid.c
 * @brief UUID version 4 (random) generation per RFC 4122.
 *
 * Generates universally unique identifiers (UUIDs) version 4 using
 * cryptographically strong randomness from /dev/urandom, with a
 * fallback to the C library rand() function when /dev/urandom is
 * unavailable.
 *
 * UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *   where '4' indicates RFC 4122 version 4 (random UUID)
 *   where 'y' has the top 2 bits set to '10' (RFC 4122 variant)
 *
 * @warning The rand() fallback is NOT cryptographically secure.
 *          On systems without /dev/urandom, a CSILK_CRYPTO_DRIVER
 *          should be configured to supply secure randomness.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/core/crypto_dispatch.h"

void
csilk_generate_uuid(char* buf)
{
	/**
	 * @brief Random byte buffer for UUID generation.
	 *
	 * UUID v4 requires 128 bits (16 bytes) of randomness:
	 *   - bytes 0-5: time_low + time_mid (48 random bits)
	 *   - byte 6:    clock_seq_hi_and_reserved (4 version bits + 4 random)
	 *   - byte 7:    clock_seq_low (8 random bits)
	 *   - byte 8:    time_hi_and_version (2 variant bits + 6 random, or
	 *                 rather in the RFC layout: 4 version + 12 random for
	 *                 time_hi_ver; but we apply the 0x40 mask on byte 6
	 *                 and 0x80 on byte 8 for the variant field placement)
	 *   - bytes 9-15: node (48 random bits)
	 */
	uint8_t random[16];
	FILE* f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(random, 1, 16, f) != 16) {
			/* urandom read failed — fall back to rand() */
			for (int i = 0; i < 16; i++) {
				random[i] = rand() & 0xFF;
			}
		}
		fclose(f);
	} else {
		/* /dev/urandom not available — fall back to rand() */
		for (int i = 0; i < 16; i++) {
			random[i] = rand() & 0xFF;
		}
	}

	/* Apply RFC 4122 §4.4 version and variant bit masks:
	 *
	 * Version (byte 6, clock_seq_hi_and_reserved):
	 *   Clear the top 4 bits (0x0F), then set to 0100 (0x40) for version 4.
	 *   Result: 0100xxxx (version 4 indicator in the high nibble).
	 *
	 * Variant (byte 8, time_hi_and_version — actually in the RFC layout
	 * this is the clock_seq_hi_and_reserved variant field; the bit
	 * placement here follows the common UUID v4 implementation pattern):
	 *   Clear the top 2 bits (0x3F), then set to 10xxxxxx (0x80) for
	 *   RFC 4122 variant.  Result: 10xxxxxx (variant 1, RFC 4122).
	 */
	random[6] = (random[6] & 0x0F) | 0x40;
	random[8] = (random[8] & 0x3F) | 0x80;

	/* Format the 16 random bytes as a 36-character UUID string with
	 * hyphens in the standard 8-4-4-4-12 grouping pattern, plus a
	 * null terminator (total 37 bytes). */
	snprintf(buf,
		 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%"
		 "02x%02x",
		 random[0],
		 random[1],
		 random[2],
		 random[3],
		 random[4],
		 random[5],
		 random[6],
		 random[7],
		 random[8],
		 random[9],
		 random[10],
		 random[11],
		 random[12],
		 random[13],
		 random[14],
		 random[15]);
}
