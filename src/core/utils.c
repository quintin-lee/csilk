/**
 * @file utils.c
 * @brief Core cryptographic and encoding utilities.
 *
 * Implements low-level building blocks used throughout the csilk framework:
 *   - SHA-1   : WebSocket handshake (RFC 6455) — intentionally weak, do NOT
 *               use for security-critical purposes.
 *   - SHA-256 : HMAC, JWT signing, session integrity — full FIPS 180-4 impl.
 *   - HMAC-SHA256 : Keyed-hash message authentication (RFC 2104) for JWT, CSRF.
 *   - Base64 / Base64URL : Encoding for JWT, WebSocket key, cookie values.
 *   - UUID v4 : Per-request unique identifiers (RFC 4122, random variant).
 *   - WebSocket frame parsing: raw frame decode for the ws middleware.
 *
 * All functions support the internal dispatch pattern: they can be called
 * standalone (using built-in software implementations) or delegating through
 * the context's crypto/cipher driver when one is set.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/drivers/cipher.h"

#ifdef TEST_OOM
int g_oom_fail_after = -1;
int g_oom_count = 0;
#endif

/** @brief 32-bit rotate-left operation (cyclic bit shift). */
#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/** @brief Internal: process a single 64-byte block through the SHA-1
 * compression function.
 *
 * Performs the SHA-1 round computation on a 512-bit message block, updating
 * the 5-word hash state. Implements the standard SHA-1 algorithm with
 * four rounds (20 steps each) using the functions f(), k constants, and
 * message schedule expansion.
 *
 * @param state [in/out] 5-element hash state array (updated in-place).
 * @param buffer 64-byte (512-bit) message block to process. */
static void
sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
	uint32_t a, b, c, d, e;
	uint32_t w[80];

	for (int i = 0; i < 16; i++) {
		w[i] = (buffer[i * 4] << 24) | (buffer[i * 4 + 1] << 16) |
		       (buffer[i * 4 + 2] << 8) | (buffer[i * 4 + 3]);
	}
	/* Message schedule expansion for rounds 16-79.
   * Each new word is the XOR of four earlier words (3, 8, 14, 16 steps back)
   * rotated left by 1 bit. This linear feedback shift mixer provides
   * avalanche across all message bits. */
	for (int i = 16; i < 80; i++) {
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	/* SHA-1 main round loop — 4 rounds of 20 steps each (80 total). */
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			/* Round 1 (0-19): Choose function  — Ch(b,c,d) = (b & c) | (~b & d)
       * Selects bits from c when b=1, from d when b=0.
       * k = floor(2^30 * sqrt(2)) = 0x5A827999. */
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		} else if (i < 40) {
			/* Round 2 (20-39): Parity function — Parity(b,c,d) = b ^ c ^ d
       * Bitwise XOR; linear mixing for diffusion.
       * k = floor(2^30 * sqrt(3)) = 0x6ED9EBA1. */
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			/* Round 3 (40-59): Majority function — Maj(b,c,d) = (b&c)|(b&d)|(c&d)
       * Returns the majority value of the three bits (1 if >= 2 bits are 1).
       * k = floor(2^30 * sqrt(5)) = 0x8F1BBCDC. */
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			/* Round 4 (60-79): Parity function (same as Round 2).
       * k = floor(2^30 * sqrt(10)) = 0xCA62C1D6. */
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		/* Round step: temp = ROTL5(a) + f + e + k + W[i],
     * then shift (a,b,c,d,e) <- (temp, a, ROTL30(b), c, d). */
		uint32_t temp = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = temp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

/** @brief Initialize a SHA-1 hashing context with the standard initial hash
 * values.
 *
 * Sets the five state words to the SHA-1 initial constants and resets the
 * bit count to zero. Must be called before the first csilk_sha1_update().
 *
 * @param context SHA-1 context to initialize (must not be NULL). */
void
csilk_sha1_init(csilk_sha1_ctx* context)
{
	context->state[0] = 0x67452301;
	context->state[1] = 0xEFCDAB89;
	context->state[2] = 0x98BADCFE;
	context->state[3] = 0x10325476;
	context->state[4] = 0xC3D2E1F0;
	context->count[0] = context->count[1] = 0;
}

/** @brief Feed data into the SHA-1 hashing context for incremental hashing.
 *
 * Processes the input data in 64-byte blocks, updating the context's state.
 * Partial blocks are buffered until the next call or csilk_sha1_final().
 *
 * @param context SHA-1 context (initialized via csilk_sha1_init()).
 * @param data    Input data buffer.
 * @param len     Length of input data in bytes.
 * @note Can be called multiple times with successive data chunks. */
void
csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len)
{
	uint32_t j = context->count[0];
	// context->count stores bytes, not bits, in the original implementation
	if ((context->count[0] += (uint32_t)len) < j) {
		context->count[1]++;
	}

	j %= 64;
	uint32_t fill = 64 - j;
	size_t i_sz;

	if (len >= fill) {
		memcpy(context->buffer + j, data, fill);
		sha1_transform(context->state, context->buffer);
		for (i_sz = fill; i_sz + 63 < len; i_sz += 64) {
			sha1_transform(context->state, data + i_sz);
		}
		j = 0;
	} else {
		i_sz = 0;
	}
	memcpy(context->buffer + j, data + i_sz, len - i_sz);
}

/** @brief Finalize the SHA-1 hash and produce the 20-byte digest.
 *
 * Pads the message according to RFC 3174 (SHA-1 specification), appends the
 * 64-bit message length, and outputs the final hash digest. After this call,
 * the context should not be used without re-initialization.
 *
 * @param context SHA-1 context with accumulated data.
 * @param digest  [out] 20-byte buffer to receive the hash digest. */
void
csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20])
{
	uint8_t finalcount[8];
	uint64_t total_bits = ((uint64_t)context->count[1] << 32 | context->count[0]) * 8;
	for (int i = 0; i < 8; i++) {
		finalcount[i] = (uint8_t)((total_bits >> (56 - i * 8)) & 0xFF);
	}

	uint32_t left = (context->count[0] % 64);
	uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
	uint8_t padding[128] = {0x80};
	csilk_sha1_update(context, padding, pad_len);
	csilk_sha1_update(context, finalcount, 8);

	for (int i = 0; i < 20; i++) {
		digest[i] = (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
	}
}

/* --- SHA256 Implementation --- */

/* 32-bit rotate-right (circular shift) — inverse of SHA-1's rol. */
#define ror(value, bits) (((value) >> (bits)) | ((value) << (32 - (bits))))

/* SHA-256 Choose function: Ch(x,y,z) = (x & y) ^ (~x & z)
 * For each bit position, selects y when x=1, z when x=0. */
#define ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))

/* SHA-256 Majority function: Maj(x,y,z) = (x&y) ^ (x&z) ^ (y&z)
 * Returns 1 when at least two of the three bits are 1. */
#define maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* SHA-256 uppercase Sigma functions (compression function rounds):
 * Sigma0(x) = ROTR^2(x) XOR ROTR^13(x) XOR ROTR^22(x) — used in T2.
 * Sigma1(x) = ROTR^6(x) XOR ROTR^11(x) XOR ROTR^25(x) — used in T1. */
#define sigma0(x) (ror(x, 2) ^ ror(x, 13) ^ ror(x, 22))
#define sigma1(x) (ror(x, 6) ^ ror(x, 11) ^ ror(x, 25))

/* SHA-256 lowercase sigma functions (message schedule expansion):
 * gamma0(x) = ROTR^7(x) XOR ROTR^18(x) XOR SHR^3(x) — for w[i-15].
 * gamma1(x) = ROTR^17(x) XOR ROTR^19(x) XOR SHR^10(x) — for w[i-2]. */
#define gamma0(x) (ror(x, 7) ^ ror(x, 18) ^ ((x) >> 3))
#define gamma1(x) (ror(x, 17) ^ ror(x, 19) ^ ((x) >> 10))

static const uint32_t k256[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/** @brief Process a single 64-byte block through the SHA-256 compression
 * function (FIPS 180-4 §6.2.2).
 *
 * The SHA-256 compression function operates on a 256-bit (8-word) state and
 * a 512-bit (64-byte) message block:
 *
 *   1. Message Schedule (w[0..63]): The first 16 words are the message block
 *      in big-endian. Words 16-63 are expanded using:
 *        w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16]
 *      where gamma0 and gamma1 are the "lowercase sigma" diffusion functions.
 *
 *   2. State initialisation: a..h = state[0..7].
 *
 *   3. Compression loop (64 rounds): Each round computes:
 *        T1 = h + Sigma1(e) + Ch(e,f,g) + K[i] + w[i]
 *        T2 = Sigma0(a) + Maj(a,b,c)
 *        a..h = (T1+T2, a, b, c, d+T1, e, f, g)
 *      The K constants are the first 32 bits of the fractional parts of the
 *      cube roots of the first 64 primes (nothing-up-my-sleeve numbers).
 *
 *   4. State update: state[n] += working variable (a..h).
 *
 * @param state [in/out] 8-element hash state (updated in-place).
 * @param data  64-byte (512-bit) message block to process. */
static void
sha256_transform(uint32_t state[8], const uint8_t data[64])
{
	uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

	/* Message schedule: first 16 words = big-endian message block */
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16 |
		       (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
	}
	/* Message schedule expansion (w[16..63]) using the lowercase sigma
   * functions for diffusion across the entire message. */
	for (int i = 16; i < 64; i++) {
		w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
	}

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	/* 64 rounds of the SHA-256 compression function.
   * Each round mixes the working variables through Sigma, Ch, Maj, and
   * the round constant K[i] for both diffusion and Confusion. */
	for (int i = 0; i < 64; i++) {
		t1 = h + sigma1(e) + ch(e, f, g) + k256[i] + w[i];
		t2 = sigma0(a) + maj(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

/** @brief Initialize a SHA-256 hashing context with the standard initial hash
 * values.
 *
 * Sets the eight state words to the SHA-256 initial constants and resets
 * the bit count to zero.
 *
 * @param context SHA-256 context to initialize (must not be NULL). */
void
csilk_sha256_init(csilk_sha256_ctx* context)
{
	context->state[0] = 0x6a09e667;
	context->state[1] = 0xbb67ae85;
	context->state[2] = 0x3c6ef372;
	context->state[3] = 0xa54ff53a;
	context->state[4] = 0x510e527f;
	context->state[5] = 0x9b05688c;
	context->state[6] = 0x1f83d9ab;
	context->state[7] = 0x5be0cd19;
	context->count = 0;
}

/** @brief Feed data into the SHA-256 hashing context for incremental hashing.
 *
 * Processes the input data in 64-byte blocks, updating the context's state.
 * Partial blocks are buffered. Tracks the total bit count for final padding.
 *
 * @param context SHA-256 context (initialized via csilk_sha256_init()).
 * @param data    Input data buffer.
 * @param len     Length of input data in bytes. */
void
csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data, size_t len)
{
	uint32_t i, idx = (uint32_t)((context->count >> 3) & 0x3F);
	context->count += (uint64_t)len << 3;

	if (64 - idx <= len) {
		memcpy(context->buffer + idx, data, 64 - idx);
		sha256_transform(context->state, context->buffer);
		for (i = 64 - idx; i + 63 < len; i += 64) {
			sha256_transform(context->state, data + i);
		}
		idx = 0;
	} else {
		i = 0;
	}
	memcpy(context->buffer + idx, data + i, len - i);
}

/** @brief Finalize the SHA-256 hash and produce the 32-byte digest.
 *
 * Pads the message according to FIPS 180-4, appends the 64-bit message
 * length, and outputs the final 256-bit (32-byte) hash digest.
 *
 * @param context SHA-256 context with accumulated data.
 * @param digest  [out] 32-byte buffer to receive the hash digest. */
void
csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32])
{
	uint8_t finalcount[8];
	for (int i = 0; i < 8; i++) {
		finalcount[i] = (uint8_t)((context->count >> (56 - i * 8)) & 0xFF);
	}

	uint32_t left = (uint32_t)((context->count >> 3) % 64);
	uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
	uint8_t padding[128] = {0x80};
	csilk_sha256_update(context, padding, pad_len);
	csilk_sha256_update(context, finalcount, 8);

	for (int i = 0; i < 32; i++) {
		digest[i] = (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
	}
}

/** @brief Compute HMAC-SHA256 as defined in RFC 2104.
 *
 * HMAC (Hash-based Message Authentication Code) provides both data integrity
 * and authenticity via a shared secret. The construction is:
 *
 *   HMAC(K, m) = SHA256((K' XOR opad) || SHA256((K' XOR ipad) || m))
 *
 * where:
 *   - K' is the key, hashed with SHA256 if longer than 64 bytes (block size).
 *   - ipad = 0x36 repeated 64 times (inner padding).
 *   - opad = 0x5C repeated 64 times (outer padding).
 *   - || denotes concatenation.
 *
 * The double-hashing protects against length-extension attacks on the
 * underlying hash function. The ipad/opad XOR ensures that the inner and
 * outer hashes use distinct keys derived from the same secret.
 *
 * @param key      HMAC secret key.
 * @param key_len  Key length in bytes.
 * @param data     Input message data.
 * @param data_len Message length in bytes.
 * @param out      [out] 32-byte output buffer for the HMAC digest. */
void
csilk_hmac_sha256(
    const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32])
{
	csilk_sha256_ctx ctx;
	uint8_t k_ipad[64], k_opad[64], tk[32];

	if (key_len > 64) {
		csilk_sha256_init(&ctx);
		csilk_sha256_update(&ctx, key, key_len);
		csilk_sha256_final(&ctx, tk);
		key = tk;
		key_len = 32;
	}

	memset(k_ipad, 0, 64);
	memset(k_opad, 0, 64);
	memcpy(k_ipad, key, key_len);
	memcpy(k_opad, key, key_len);

	for (int i = 0; i < 64; i++) {
		k_ipad[i] ^= 0x36;
		k_opad[i] ^= 0x5c;
	}

	csilk_sha256_init(&ctx);
	csilk_sha256_update(&ctx, k_ipad, 64);
	csilk_sha256_update(&ctx, data, data_len);
	csilk_sha256_final(&ctx, out);

	csilk_sha256_init(&ctx);
	csilk_sha256_update(&ctx, k_opad, 64);
	csilk_sha256_update(&ctx, out, 32);
	csilk_sha256_final(&ctx, out);
}

/** @brief Standard Base64 alphabet per RFC 4648 §4.
 *
 * The alphabet uses 64 ASCII characters: A-Z (indices 0-25), a-z (26-51),
 * 0-9 (52-61), '+' (62), '/' (63). Each group of 3 input bytes (24 bits)
 * is encoded as 4 Base64 characters (6 bits each). If the input length is
 * not a multiple of 3, padding '=' characters are added. */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** @brief Encode raw bytes as a standard Base64 string per RFC 4648.
 *
 * Processes input in 3-byte groups, producing 4 Base64 characters each.
 * Padding with '=' is added if the input length is not a multiple of 3.
 * The output string is null-terminated.
 *
 * @param src Input byte buffer.
 * @param len Input length in bytes.
 * @param out [out] Output buffer (must be large enough: 4 * ceil(len/3) + 1).
 * @note The caller must ensure @p out has sufficient capacity. The worst-case
 *       output length is ((len + 2) / 3) * 4 + 1. */
void
csilk_base64_encode(const uint8_t* src, size_t len, char* out)
{
	/* Process input in 3-byte groups (24 bits), producing 4 Base64 chars.
   * For a partial final group (1 or 2 bytes), missing bytes become '='. */
	size_t i, j;
	for (i = 0, j = 0; i < len; i += 3) {
		/* Pack up to 3 bytes into a 24-bit value (big-endian).
     * Missing bytes for the final group are 0. */
		uint32_t v = src[i] << 16;
		if (i + 1 < len) {
			v |= src[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= src[i + 2];
		}
		/* Extract four 6-bit indices into the Base64 table */
		out[j++] = b64_table[(v >> 18) & 0x3F];
		out[j++] = b64_table[(v >> 12) & 0x3F];
		if (i + 1 < len) {
			out[j++] = b64_table[(v >> 6) & 0x3F];
		} else {
			out[j++] = '=';
		}
		if (i + 2 < len) {
			out[j++] = b64_table[v & 0x3F];
		} else {
			out[j++] = '=';
		}
	}
	out[j] = '\0';
}

/** @brief Encode raw bytes as a Base64URL string per RFC 4648 §5 (URL-safe).
 *
 * Base64URL is the same as standard Base64 but replaces:
 *   '+' → '-'  (URL-safe, as '+' is treated as space in URL query strings)
 *   '/' → '_'  (URL-safe, as '/' has path separator meaning)
 *   '=' → ''   (omitted — padding is unnecessary because length is inferred)
 *
 * The output is produced by first encoding with standard Base64, then
 * character-substituting and stripping padding.
 *
 * @param src Input byte buffer.
 * @param len Input length in bytes.
 * @param out [out] Output buffer (must be large enough for the padded
 *           Base64 result + 1).
 * @note The output is NOT padded with '='. The length can be inferred from
 *       strlen(out). */
void
csilk_base64url_encode(const uint8_t* src, size_t len, char* out)
{
	csilk_base64_encode(src, len, out);
	for (char* p = out; *p; p++) {
		if (*p == '+') {
			*p = '-';
		} else if (*p == '/') {
			*p = '_';
		} else if (*p == '=') {
			*p = '\0';
			break;
		}
	}
}

/** @brief Decode a Base64URL-encoded string back to raw bytes.
 *
 * The decoding process is the inverse of Base64URL encoding:
 *   1. Replace URL-safe characters ('-', '_') with standard Base64 chars
 *      ('+', '/').
 *   2. Restore padding '=' characters so the length is a multiple of 4.
 *   3. Decode the resulting standard Base64 using a reverse lookup table.
 *
 * The reverse lookup maps each Base64 character (A-Z, a-z, 0-9, +, /) back
 * to its 6-bit value. Characters outside this set (including whitespace)
 * cause an immediate error return (-1). The '=' padding character terminates
 * decoding early.
 *
 * @param src Base64URL-encoded input string (null-terminated).
 * @param out [out] Output buffer for decoded bytes.
 * @return The number of decoded bytes on success, or -1 on invalid input
 *         (non-Base64 characters) or allocation failure.
 * @note The caller should ensure @p out is large enough (at least
 *       strlen(src) * 3 / 4 + 1 bytes). */
int
csilk_base64url_decode(const char* src, uint8_t* out)
{
	size_t len = strlen(src);
	char* tmp = malloc(len + 5);
	if (!tmp) {
		return -1;
	}
	strcpy(tmp, src);
	for (size_t i = 0; i < len; i++) {
		if (tmp[i] == '-') {
			tmp[i] = '+';
		} else if (tmp[i] == '_') {
			tmp[i] = '/';
		}
	}
	while (len % 4) {
		tmp[len++] = '=';
	}
	tmp[len] = '\0';

	/* Reverse lookup table: maps ASCII characters to their 6-bit Base64 values.
   * -1 means invalid (not a Base64 character). The table covers all 256
   * possible byte values for O(1) lookup without conditional branches. */
	static const int8_t b64_rev_table[256] = {
	    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
	    -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,
	    1,	2,  3,	4,  5,	6,  7,	8,  9,	10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
	    23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
	    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};

	/* Bit-stream decoder: accumulates Base64 6-bit groups into a 32-bit buffer,
   * then extracts full bytes (8 bits) from the buffer as they become available.
   * This approach naturally handles a final group with fewer than 4 characters
   * (after padding restoration), because '=' terminates the loop early. */
	int decoded_len = 0;
	uint32_t v = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		if (tmp[i] == '=') {
			break;
		}
		int val = b64_rev_table[(uint8_t)tmp[i]];
		if (val < 0) {
			free(tmp);
			return -1;
		}
		v = (v << 6) | val;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[decoded_len++] = (uint8_t)((v >> bits) & 0xFF);
		}
	}
	free(tmp);
	return decoded_len;
}

/** @brief Generate a random UUID version 4 string in the standard 8-4-4-4-12
 * format.
 *
 * UUID v4 (RFC 4122 §4.4) uses random or pseudo-random bytes for all 128 bits,
 * with specific bits reserved for the version and variant:
 *
 *   Field         Bits   Purpose
 *   time_low       32    Random
 *   time_mid       16    Random
 *   time_hi_ver    16    Version (4 bits) + random (12 bits)
 *   clock_seq_hi   8     Variant (2 bits) + random (6 bits)
 *   clock_seq_low  8     Random
 *   node           48    Random
 *
 * Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *   where '4' indicates RFC 4122 version 4 (random UUID).
 *   where 'y' has the top 2 bits set to '10' (RFC 4122 variant).
 *
 * Reads 16 random bytes from /dev/urandom. If /dev/urandom is unavailable,
 * falls back to rand() (which is NOT cryptographically secure). Sets the
 * UUID version nibble (4) and variant bits (10xx) per RFC 4122.
 *
 * @param buf [out] 37-byte buffer to receive the UUID string
 *            (36 hex chars + 4 hyphens + null terminator).
 * @note The output format is: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 * @warning The fallback to rand() is NOT cryptographically secure. On systems
 *          without /dev/urandom, CSILK_CRYPTO_DRIVER should supply randomness.
 */
void
csilk_generate_uuid(char* buf)
{
	uint8_t random[16];
	FILE* f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(random, 1, 16, f) != 16) {
			/* Fallback to rand if urandom fails */
			for (int i = 0; i < 16; i++) {
				random[i] = rand() & 0xFF;
			}
		}
		fclose(f);
	} else {
		for (int i = 0; i < 16; i++) {
			random[i] = rand() & 0xFF;
		}
	}

	/* Per RFC 4122 §4.4:
   *   - byte 6  (clock_seq_hi_and_reserved): set top 4 bits to 0100 (version 4)
   *   - byte 8  (time_hi_and_version):       set top 2 bits to 10   (variant 1)
   *
   *   random[6] = (random[6] & 0x0F) | 0x40  — clears top 4 bits, sets 0100
   *   random[8] = (random[8] & 0x3F) | 0x80  — clears top 2 bits, sets 10 */
	random[6] = (random[6] & 0x0F) | 0x40;
	random[8] = (random[8] & 0x3F) | 0x80;

	/* Format as: %08x-%04x-%04x-%04x-%012x */
	sprintf(buf,
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

/** @brief Context-aware HMAC-SHA256 — delegates to the crypto driver if
 * available.
 *
 * This is the "late-bound" version of csilk_hmac_sha256(). It checks whether
 * the request context has a crypto driver installed (e.g., OpenSSL, mbedTLS,
 * or a hardware security module). If so, the driver's accelerated HMAC is
 * used. Otherwise, the built-in software implementation serves as the
 * portable fallback.
 *
 * This pattern allows the application to use pluggable crypto backends
 * without changing caller code. The default built-in implementation is
 * always available for environments without hardware crypto.
 *
 * @param c        Request context (may be NULL — falls back to built-in).
 * @param key      HMAC key.
 * @param key_len  Key length.
 * @param data     Input data.
 * @param data_len Data length.
 * @param out      [out] 32-byte HMAC output buffer. */
void
_csilk_hmac_sha256(csilk_ctx_t* c,
		   const uint8_t* key,
		   size_t key_len,
		   const uint8_t* data,
		   size_t data_len,
		   uint8_t out[32])
{
	if (c && c->crypto_driver && c->crypto_driver->hmac_sha256) {
		c->crypto_driver->hmac_sha256(key, key_len, data, data_len, out);
	} else {
		csilk_hmac_sha256(key, key_len, data, data_len, out);
	}
}

/** @brief Context-aware UUID generation — delegates to the crypto driver if
 * available.
 *
 * This is the late-bound UUID generator. If the context has a crypto driver
 * with a cryptographically secure generate_uuid method (e.g., reading from
 * a hardware RNG or via OpenSSL), that is used. Otherwise falls back to the
 * built-in csilk_generate_uuid() which reads /dev/urandom.
 *
 * The delegation pattern ensures callers always get the best available
 * randomness source without explicit driver management.
 *
 * @param c   Request context (may be NULL — falls back to built-in).
 * @param buf [out] 37-byte buffer for the UUID string. */
void
_csilk_generate_uuid(csilk_ctx_t* c, char buf[37])
{
	if (c && c->crypto_driver && c->crypto_driver->generate_uuid) {
		c->crypto_driver->generate_uuid(buf);
	} else {
		csilk_generate_uuid(buf);
	}
}

extern csilk_cipher_driver_t csilk_default_cipher_driver;

static csilk_cipher_driver_t*
resolve_cipher(csilk_ctx_t* c)
{
	if (c && c->cipher_driver) {
		return c->cipher_driver;
	}
	return &csilk_default_cipher_driver;
}

int
_csilk_symmetric_encrypt(csilk_ctx_t* c,
			 const uint8_t* key,
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
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->symmetric_encrypt) {
		return -1;
	}
	return d->symmetric_encrypt(key,
				    key_len,
				    plaintext,
				    plaintext_len,
				    iv,
				    iv_len,
				    ciphertext,
				    ciphertext_len,
				    tag,
				    tag_len);
}

int
_csilk_symmetric_decrypt(csilk_ctx_t* c,
			 const uint8_t* key,
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
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->symmetric_decrypt) {
		return -1;
	}
	return d->symmetric_decrypt(key,
				    key_len,
				    ciphertext,
				    ciphertext_len,
				    iv,
				    iv_len,
				    tag,
				    tag_len,
				    plaintext,
				    plaintext_len);
}

int
_csilk_generate_keypair(
    csilk_ctx_t* c, char* public_key, size_t* pub_len, char* private_key, size_t* priv_len)
{
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->generate_keypair) {
		return -1;
	}
	return d->generate_keypair(public_key, pub_len, private_key, priv_len);
}

int
_csilk_asymmetric_encrypt(csilk_ctx_t* c,
			  const char* public_key,
			  size_t pub_len,
			  const uint8_t* plaintext,
			  size_t plaintext_len,
			  uint8_t* ciphertext,
			  size_t* ciphertext_len)
{
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->asymmetric_encrypt) {
		return -1;
	}
	return d->asymmetric_encrypt(
	    public_key, pub_len, plaintext, plaintext_len, ciphertext, ciphertext_len);
}

int
_csilk_asymmetric_decrypt(csilk_ctx_t* c,
			  const char* private_key,
			  size_t priv_len,
			  const uint8_t* ciphertext,
			  size_t ciphertext_len,
			  uint8_t* plaintext,
			  size_t* plaintext_len)
{
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->asymmetric_decrypt) {
		return -1;
	}
	return d->asymmetric_decrypt(
	    private_key, priv_len, ciphertext, ciphertext_len, plaintext, plaintext_len);
}

int
_csilk_sign(csilk_ctx_t* c,
	    const char* private_key,
	    size_t priv_len,
	    const uint8_t* data,
	    size_t data_len,
	    uint8_t* signature,
	    size_t* sig_len)
{
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->sign) {
		return -1;
	}
	return d->sign(private_key, priv_len, data, data_len, signature, sig_len);
}

int
_csilk_verify(csilk_ctx_t* c,
	      const char* public_key,
	      size_t pub_len,
	      const uint8_t* data,
	      size_t data_len,
	      const uint8_t* signature,
	      size_t sig_len)
{
	csilk_cipher_driver_t* d = resolve_cipher(c);
	if (!d || !d->verify) {
		return -1;
	}
	return d->verify(public_key, pub_len, data, data_len, signature, sig_len);
}
