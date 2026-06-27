/**
 * @file base64.c
 * @brief Base64 and Base64URL encoding/decoding (RFC 4648).
 *
 * Implements standard Base64 encoding (RFC 4648 §4) and URL-safe Base64URL
 * encoding/decoding (RFC 4648 §5).  Used throughout the csilk framework
 * for JWT serialisation, WebSocket handshake key encoding, cookie value
 * encoding, and any context where binary-to-text conversion is required.
 *
 * Standard Base64 uses: A-Z, a-z, 0-9, +, /  with '=' padding.
 * Base64URL substitutes: '+' → '-', '/' → '_', and omits padding.
 *
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/codec.h"

/**
 * @brief Standard Base64 alphabet per RFC 4648 §4.
 *
 * Maps 6-bit values (0-63) to their ASCII representation:
 *   Indices  0-25: A-Z
 *   Indices 26-51: a-z
 *   Indices 52-61: 0-9
 *   Index      62: '+'
 *   Index      63: '/'
 *
 * Each 3-byte input group (24 bits) produces 4 Base64 characters (6 bits each).
 */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** @brief Encode binary data to standard Base64 (RFC 4648 §4).
 *
 * The output buffer must be large enough to hold the encoded string plus
 * NUL terminator.  The required size is ((len + 2) / 3) * 4 + 1 bytes.
 *
 * @param src  Input binary data.
 * @param len  Number of bytes to encode.
 * @param[out] out  Output buffer (caller-allocated, see sizing note above). */
void
csilk_base64_encode(const uint8_t* src, size_t len, char* out)
{
	/* Process input in 3-byte groups, emitting 4 Base64 characters per group.
	 * A partial final group (1 or 2 bytes) is padded with '=' characters to
	 * ensure the output length is always a multiple of 4. */
	size_t i, j;
	for (i = 0, j = 0; i < len; i += 3) {
		/* Pack up to 3 bytes into a 24-bit big-endian value.
		 * Missing bytes for a partial final group are treated as zero,
		 * which will produce '=' padding in the corresponding positions. */
		uint32_t v = src[i] << 16;
		if (i + 1 < len) {
			v |= src[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= src[i + 2];
		}
		/* Extract four 6-bit indices into the Base64 alphabet table */
		out[j++] = b64_table[(v >> 18) & 0x3F];
		out[j++] = b64_table[(v >> 12) & 0x3F];
		if (i + 1 < len) {
			out[j++] = b64_table[(v >> 6) & 0x3F];
		} else {
			out[j++] = '='; /* padding for missing second byte */
		}
		if (i + 2 < len) {
			out[j++] = b64_table[v & 0x3F];
		} else {
			out[j++] = '='; /* padding for missing third byte */
		}
	}
	out[j] = '\0';
}

/** @brief Encode binary data to Base64URL (RFC 4648 §5).
 *
 * Same as csilk_base64_encode() but substitutes '+' → '-', '/' → '_',
 * and omits trailing '=' padding, making the result safe for use in
 * URL query strings and JWT payloads without additional escaping.
 *
 * @param src  Input binary data.
 * @param len  Number of bytes to encode.
 * @param[out] out  Output buffer (same size as standard Base64). */
void
csilk_base64url_encode(const uint8_t* src, size_t len, char* out)
{
	/* First produce standard Base64, then apply URL-safe character
	 * substitutions and strip padding.  This two-pass approach keeps
	 * the encoding logic simple and avoids duplicating the core loop. */
	csilk_base64_encode(src, len, out);
	for (char* p = out; *p; p++) {
		if (*p == '+') {
			*p = '-'; /* '+' is ambiguous in URL query strings */
		} else if (*p == '/') {
			*p = '_'; /* '/' conflicts with URL path separators */
		} else if (*p == '=') {
			*p = '\0'; /* padding is unnecessary; length is inferable */
			break;
		}
	}
}

/** @brief Decode a Base64URL string back to raw bytes (RFC 4648 §5).
 *
 * Reverses the URL-safe character substitutions ('-' → '+', '_' → '/'),
 * restores padding, then decodes via a reverse-lookup table and bit-stream
 * decoder.  Invalid characters or buffer overflow cause an early return.
 *
 * @param src     Null-terminated Base64URL input string.
 * @param[out] out  Decoded binary output (caller-allocated).
 * @param out_cap   Maximum bytes that can be written to @p out.
 * @return The number of decoded bytes on success, or -1 on invalid input
 *         or buffer overflow. */
int
csilk_base64url_decode(const char* src, uint8_t* out, size_t out_cap)
{
	/* Decoding proceeds in three steps:
	 *   1. Convert URL-safe characters back to standard Base64.
	 *   2. Restore padding so the length is a multiple of 4.
	 *   3. Decode using a reverse-lookup table and bit-stream decoder. */
	size_t len = strlen(src);
	char* tmp = malloc(len + 5);
	if (!tmp) {
		return -1;
	}
	memcpy(tmp, src, len + 1);
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

	/*
	 * Reverse lookup table for O(1) Base64 character decoding.
	 * See the static array definition below for the layout.
	 */
	static const int8_t b64_rev_table[256] = {
	    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
	    -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,
	    1,	2,  3,	4,  5,	6,  7,	8,  9,	10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
	    23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
	    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};

	/* Bit-stream decoder: Accumulates 6-bit Base64 groups into a 32-bit
	 * buffer and extracts complete 8-bit output bytes as they become
	 * available.  When the accumulator holds at least 8 bits, the top 8
	 * bits are extracted and written to the output.
	 *
	 * This approach naturally handles partial final groups: the '='
	 * padding character terminates the loop early, and any remaining
	 * bits in the accumulator that are less than 8 are discarded
	 * (they correspond to the padding bits, which should be zero). */
	int decoded_len = 0;
	uint32_t v = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		if (tmp[i] == '=') {
			break; /* padding terminates the data */
		}
		int val = b64_rev_table[(uint8_t)tmp[i]];
		if (val < 0) {
			free(tmp);
			return -1; /* invalid Base64 character */
		}
		v = (v << 6) | val;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if ((size_t)decoded_len >= out_cap) {
				free(tmp);
				return -1; /* output buffer capacity exceeded */
			}
			out[decoded_len++] = (uint8_t)((v >> bits) & 0xFF);
		}
	}
	free(tmp);
	return decoded_len;
}
