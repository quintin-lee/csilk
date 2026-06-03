/**
 * @file sha1.c
 * @brief SHA-1 hash implementation (RFC 3174) for WebSocket handshake.
 *
 * Implements the SHA-1 cryptographic hash function producing a 160-bit
 * (20-byte) digest.  Used exclusively for WebSocket handshake key
 * verification per RFC 6455.
 *
 * @warning SHA-1 is cryptographically broken.  Do NOT use for security-
 *          critical purposes.  This implementation exists only for
 *          WebSocket protocol compliance.
 * @copyright MIT License
 */

#include <stdint.h>
#include <string.h>

#include "csilk/core/hash.h"

/**
 * @brief 32-bit rotate-left operation.
 *
 * Cyclically shifts a 32-bit value left by the specified number of bits.
 * Bits shifted off the left end are wrapped around to the right.
 * This is the fundamental diffusion operation in SHA-1's compression
 * function, used both in message schedule expansion (ROTL1) and in
 * the round computation (ROTL5, ROTL30).
 *
 * @param value The 32-bit value to rotate.
 * @param bits  Number of bit positions to rotate left (0-31).
 */
#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/**
 * @brief Process a single 64-byte block through the SHA-1 compression function.
 *
 * The SHA-1 compression function (FIPS 180-4 §6.1.2) operates on a 160-bit
 * (5-word) hash state and a 512-bit (64-byte) message block:
 *
 *   1. Message Schedule (w[0..79]): The first 16 words are the message block
 *      in big-endian byte order.  Words 16-79 are expanded using a linear
 *      feedback shift: w[i] = ROTL1(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]).
 *
 *   2. Working variable initialisation: a..e = state[0..4].
 *
 *   3. Compression loop (80 rounds in 4 stages):
 *      - Rounds 0-19  (f = Ch):  (b & c) | (~b & d),  K = 0x5A827999
 *      - Rounds 20-39 (f = Parity):  b ^ c ^ d,       K = 0x6ED9EBA1
 *      - Rounds 40-59 (f = Maj):  (b & c) | (b & d) | (c & d), K = 0x8F1BBCDC
 *      - Rounds 60-79 (f = Parity):  b ^ c ^ d,       K = 0xCA62C1D6
 *      Each round: temp = ROTL5(a) + f + e + K + w[i],
 *      then rotate: (a,b,c,d,e) <- (temp, a, ROTL30(b), c, d)
 *
 *   4. State update: state[n] += working variable.
 *
 * The Ch function selects bits from c (when b=1) or d (when b=0).
 * The Maj function returns the majority bit value.
 * The Parity function provides linear diffusion.
 * The K constants are derived from the fractional parts of the square
 * roots of small primes (nothing-up-my-sleeve numbers).
 *
 * @param state  [in/out] 5-element hash state array (updated in-place).
 * @param buffer 64-byte (512-bit) message block to process.
 */
static void
sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
	uint32_t a, b, c, d, e;
	uint32_t w[80];

	/* Load big-endian message block into the first 16 schedule words */
	for (int i = 0; i < 16; i++) {
		w[i] = (buffer[i * 4] << 24) | (buffer[i * 4 + 1] << 16) |
		       (buffer[i * 4 + 2] << 8) | (buffer[i * 4 + 3]);
	}
	/* Message schedule expansion: linear feedback shift for avalanche */
	for (int i = 16; i < 80; i++) {
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}

	/* Initialise working variables from current hash state */
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	/* 80-round compression function (4 stages × 20 rounds) */
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			/* Round 1: Choose function — Ch(b,c,d) */
			f = (b & c) | ((~b) & d);
			k = 0x5A827999; /* floor(2^30 * sqrt(2)) */
		} else if (i < 40) {
			/* Round 2: Parity function */
			f = b ^ c ^ d;
			k = 0x6ED9EBA1; /* floor(2^30 * sqrt(3)) */
		} else if (i < 60) {
			/* Round 3: Majority function — Maj(b,c,d) */
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC; /* floor(2^30 * sqrt(5)) */
		} else {
			/* Round 4: Parity function (same as Round 2) */
			f = b ^ c ^ d;
			k = 0xCA62C1D6; /* floor(2^30 * sqrt(10)) */
		}
		uint32_t temp = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = temp;
	}

	/* Add the compressed result back to the hash state */
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

void
csilk_sha1_init(csilk_sha1_ctx* context)
{
	/* Standard SHA-1 initial hash values (FIPS 180-4 §5.3.1):
	 * These are the fractional parts of the square roots of the
	 * first five primes (2, 3, 5, 7, 11), providing a nothing-up-my-sleeve
	 * starting state that ensures no single fixed input dominates. */
	context->state[0] = 0x67452301;
	context->state[1] = 0xEFCDAB89;
	context->state[2] = 0x98BADCFE;
	context->state[3] = 0x10325476;
	context->state[4] = 0xC3D2E1F0;
	context->count[0] = context->count[1] = 0;
}

void
csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len)
{
	/* Update total byte count with overflow detection (64-bit counter
	 * stored as two 32-bit halves for platforms without uint64_t) */
	uint32_t j = context->count[0];
	if ((context->count[0] += (uint32_t)len) < j) {
		context->count[1]++;
	}

	/* Process any complete 64-byte blocks, buffering the remainder */
	j %= 64;
	uint32_t fill = 64 - j;
	size_t i_sz;

	if (len >= fill) {
		/* Fill the current buffer and process it */
		memcpy(context->buffer + j, data, fill);
		sha1_transform(context->state, context->buffer);
		/* Process remaining full blocks directly */
		for (i_sz = fill; i_sz + 63 < len; i_sz += 64) {
			sha1_transform(context->state, data + i_sz);
		}
		j = 0;
	} else {
		i_sz = 0;
	}
	/* Buffer any remaining partial block */
	memcpy(context->buffer + j, data + i_sz, len - i_sz);
}

void
csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20])
{
	/* Encode the total message length in bits as a big-endian 64-bit value */
	uint8_t finalcount[8];
	uint64_t total_bits = ((uint64_t)context->count[1] << 32 | context->count[0]) * 8;
	for (int i = 0; i < 8; i++) {
		finalcount[i] = (uint8_t)((total_bits >> (56 - i * 8)) & 0xFF);
	}

	/* SHA-1 padding: append a 1-bit (0x80), then zero bits until the
	 * message length ≡ 448 mod 512 (56 mod 64), then append the 64-bit
	 * length.  If there are fewer than 8 bytes left in the current block,
	 * an extra block of padding is required. */
	uint32_t left = (context->count[0] % 64);
	uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
	uint8_t padding[128] = {0x80};
	csilk_sha1_update(context, padding, pad_len);
	csilk_sha1_update(context, finalcount, 8);

	/* Extract the 160-bit digest from the state array in big-endian byte order */
	for (int i = 0; i < 20; i++) {
		digest[i] = (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
	}
}
