/**
 * @file uuid.c
 * @brief UUID version 4 (random) generation per RFC 4122.
 *
 * Generates universally unique identifiers (UUIDs) version 4 using
 * cryptographically strong randomness.
 *
 * UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *   where '4' indicates RFC 4122 version 4 (random UUID)
 *   where 'y' has the top 2 bits set to '10' (RFC 4122 variant)
 *
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/crypto_dispatch.h"
#include "csilk/core/internal.h"
#include "csilk/crypto.h"

/** @brief Generate a version-4 (random) UUID string per RFC 4122.
 *
 * Fills @p buf with a 36-character UUID string (plus NUL terminator,
 * total CSILK_UUID_BUF_SIZE bytes) of the form:
 *   xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *
 * The random bytes are sourced from csilk_crypto_fill_random().  On
 * failure (no entropy) the function calls abort() — production callers
 * should ensure the crypto subsystem is initialized before calling this.
 *
 * @param[out] buf  Output buffer, must be at least CSILK_UUID_BUF_SIZE bytes. */
void
csilk_generate_uuid(char* buf)
{
	uint8_t random[16];
	if (csilk_crypto_fill_random(random, 16) != 0) {
		CSILK_LOG_F("FATAL: csilk_generate_uuid failed to get entropy.");
		abort();
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
	 * null terminator (total CSILK_UUID_BUF_SIZE bytes). */
	snprintf(buf,
		 CSILK_UUID_BUF_SIZE,
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
