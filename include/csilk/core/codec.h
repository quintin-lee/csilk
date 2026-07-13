#pragma once
/**
 * @file codec.h
 * @brief Encoding/decoding primitives — Base64, Base64URL, URL percent-decode.
 *
 * Provides encoding and decoding functions used internally by the csilk
 * framework for cookie serialization (Base64URL), HTTP header encoding,
 * and WebSocket handshake key computation (Base64).
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Encode raw bytes as a standard Base64 string.
 *
 * Produces a NUL-terminated Base64 string per RFC 4648 §4.
 *
 * @param src  Source byte buffer.
 * @param len  Byte length of @p src.
 * @param[out] out  Output buffer.  Must be at least ((len + 2) / 3) * 4 + 1
 *                  bytes to hold the encoded output plus NUL terminator.
 */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out);

/**
 * @brief Encode raw bytes as a Base64URL (RFC 4648 §5) string.
 *
 * Like Base64 but uses '-' and '_' instead of '+' and '/', and omits
 * padding '=' characters.
 *
 * @param src  Source byte buffer.
 * @param len  Byte length of @p src.
 * @param[out] out  Output buffer (must be large enough for the encoded string).
 */
void csilk_base64url_encode(const uint8_t* src, size_t len, char* out);

/**
 * @brief Decode a Base64URL (RFC 4648 §5) string to raw bytes.
 *
 * Handles both padded and unpadded input.
 *
 * @param src  NUL-terminated Base64URL string.
 * @param[out] out  Output buffer for decoded bytes (must be at least
 *                  strlen(src) * 3 / 4 bytes).
 * @param out_cap   Maximum capacity of the output buffer.
 * @return The number of decoded bytes on success, or -1 if the input
 *         contains invalid characters, the length is invalid, or the
 *         decoded output would exceed out_cap.
 */
int csilk_base64url_decode(const char* src, uint8_t* out, size_t out_cap);

/**
 * @brief URL-decode a percent-encoded string in-place.
 *
 * Converts %XX sequences to their byte values and '+' to space.
 * The decoded string is always shorter than or equal to the input.
 *
 * @param str  NUL-terminated percent-encoded string (modified in-place).
 * @return The length of the decoded string (may be shorter than strlen
 *         of the original).
 */
size_t csilk_url_decode(char* str);
