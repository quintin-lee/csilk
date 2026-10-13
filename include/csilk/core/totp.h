#pragma once
/**
 * @file csilk/core/totp.h
 * @brief Time-Based One-Time Password (TOTP, RFC 6238) and 2FA authentication engine.
 *
 * Provides Base32 key generation, dynamic 6-digit TOTP calculation,
 * sliding window verification (±30s), Key URI creation, and one-time
 * backup code generation and consumption.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a random 160-bit (20-byte) Base32 encoded TOTP secret key.
 *
 * @param[out] out_secret Output buffer to receive null-terminated Base32 string (32 chars + '\0').
 * @param[in]  cap        Capacity of output buffer (must be at least 33 bytes).
 * @return 0 on success, -1 on failure.
 */
int csilk_totp_generate_secret(char* out_secret, size_t cap);

/**
 * @brief Compute a 6-digit TOTP code for a given timestamp and Base32 secret.
 *
 * Follows RFC 6238 / RFC 4226:
 * - 30-second time step.
 * - HMAC-SHA1.
 * - Dynamic truncation mod 1,000,000 formatted as 6 digits.
 *
 * @param[in]  base32_secret Base32 secret string.
 * @param[in]  timestamp     Unix timestamp in seconds.
 * @param[out] out_code      Output buffer (at least 7 bytes).
 * @param[in]  cap           Buffer capacity.
 * @return 0 on success, -1 on failure.
 */
int
csilk_totp_generate_code(const char* base32_secret, uint64_t timestamp, char* out_code, size_t cap);

/**
 * @brief Verify a user-provided 6-digit TOTP code against a Base32 secret.
 *
 * Allows current time step and ±1 step (±30s clock skew tolerance).
 *
 * @param[in] base32_secret Base32 secret string.
 * @param[in] code          6-digit numeric string.
 * @return true if valid, false otherwise.
 */
bool csilk_totp_verify_code(const char* base32_secret, const char* code);

/**
 * @brief Build a standard otpauth:// Key URI for authenticator QR codes.
 *
 * Format: otpauth://totp/{issuer}:{account_name}?secret={secret}&issuer={issuer}&digits=6&period=30
 *
 * @param[in]  issuer       Application / service name.
 * @param[in]  account_name User identifier or email.
 * @param[in]  secret       Base32 secret key.
 * @param[out] out_uri      Output buffer.
 * @param[in]  cap          Buffer capacity.
 * @return 0 on success, -1 on buffer overflow or invalid arguments.
 */
int csilk_totp_build_uri(
    const char* issuer, const char* account_name, const char* secret, char* out_uri, size_t cap);

/**
 * @brief Generate 8 alphanumeric one-time backup recovery codes.
 *
 * Format: "xxxx-xxxx" (9 characters + null terminator per code).
 *
 * @param[out] out_codes Output array of 8 x 16-byte buffers.
 * @return 0 on success, -1 on failure.
 */
int csilk_totp_generate_backup_codes(char out_codes[8][16]);

/**
 * @brief Verify and atomically consume a backup recovery code from a JSON array string.
 *
 * Checks if input_code matches any code in backup_codes_json (supporting with or without hyphen).
 * If matched, serializes remaining codes into out_updated_json and returns true.
 *
 * @param[in]  backup_codes_json JSON array of backup codes string (e.g. ["xxxx-xxxx", ...]).
 * @param[in]  input_code        Code entered by user.
 * @param[out] out_updated_json  Buffer to receive updated JSON array string.
 * @param[in]  cap               Capacity of out_updated_json buffer.
 * @return true if verified and consumed, false otherwise.
 */
bool csilk_totp_verify_and_consume_backup_code(const char* backup_codes_json,
                                               const char* input_code,
                                               char*       out_updated_json,
                                               size_t      cap);

#ifdef __cplusplus
}
#endif
