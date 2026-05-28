/**
 * @file jwt.c
 * @brief JWT (JSON Web Token) generation and verification middleware.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief JSON-encoded JWT header used for all tokens.
 *
 * The header is fixed to {"alg":"HS256","typ":"JWT"} (HS256 = HMAC-SHA256).
 * This string is base64url-encoded during token generation.
 */
static const char* JWT_HEADER = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

/**
 * @brief Generate a signed HS256 JWT token.
 *
 * Constructs a JWT with the fixed header `{"alg":"HS256","typ":"JWT"}` and
 * the caller-supplied cJSON payload. The token is signed using HMAC-SHA256
 * with the provided secret. Every component (header, payload, signature) is
 * base64url-encoded per RFC 4648 §5.
 *
 * @param c       The request context (used for HMAC operations).
 * @param payload A cJSON object containing the claims. Must not be NULL.
 * @param secret  The HMAC-SHA256 signing secret. Must not be NULL.
 *
 * @return A newly allocated, null-terminated JWT string in the format
 *         `header.payload.signature`, or NULL on allocation failure or
 *         invalid arguments.
 *
 * @note The caller is responsible for freeing the returned string with
 *       free().
 * @warning The payload is NOT deep-copied during generation. The caller
 *          retains ownership and should free it after this function returns.
 */
char*
csilk_jwt_generate(csilk_ctx_t* c, cJSON* payload, const char* secret)
{
	if (!payload || !secret) {
		return NULL;
	}

	char* header_b64 = NULL;
	char* payload_b64 = NULL;
	char* token = NULL;

	/* Step 1: Base64url-encode the fixed JWT header.
     The header declares HS256 algorithm and JWT type per RFC 7519 §5. */
	size_t h_len = strlen(JWT_HEADER);
	size_t h_b64_len = ((h_len + 2) / 3) * 4 + 1;
	header_b64 = malloc(h_b64_len);
	if (!header_b64) {
		return NULL;
	}
	csilk_base64url_encode((const uint8_t*)JWT_HEADER, h_len, header_b64);

	/* Step 2: Serialize the cJSON payload to an unformatted JSON string,
     then base64url-encode it per RFC 4648 §5 (no padding). */
	char* payload_str = cJSON_PrintUnformatted(payload);
	if (!payload_str) {
		free(header_b64);
		return NULL;
	}
	size_t p_len = strlen(payload_str);
	size_t p_b64_len = ((p_len + 2) / 3) * 4 + 1;
	payload_b64 = malloc(p_b64_len);
	if (!payload_b64) {
		free(header_b64);
		free(payload_str);
		return NULL;
	}
	csilk_base64url_encode((const uint8_t*)payload_str, p_len, payload_b64);
	free(payload_str);

	/* Step 3: Concatenate header.payload with a single dot separator.
     This signing input is the "JWT Signature Input" per RFC 7515 §5.1. */
	size_t sign_input_len = strlen(header_b64) + 1 + strlen(payload_b64) + 1;
	char* sign_input = malloc(sign_input_len);
	if (!sign_input) {
		free(header_b64);
		free(payload_b64);
		return NULL;
	}
	sprintf(sign_input, "%s.%s", header_b64, payload_b64);

	/* Step 4: Compute HMAC-SHA256 signature over the signing input.
     Output is a 32-byte digest which is then base64url-encoded. */
	uint8_t sig[32];
	_csilk_hmac_sha256(c,
			   (const uint8_t*)secret,
			   strlen(secret),
			   (const uint8_t*)sign_input,
			   strlen(sign_input),
			   sig);

	char sig_b64[45]; /* 32 bytes -> 43 base64url chars + padding + null */
	csilk_base64url_encode(sig, 32, sig_b64);

	/* Step 5: Assemble final JWT: header.payload.signature. */
	token = malloc(strlen(sign_input) + 1 + strlen(sig_b64) + 1);
	if (token) {
		sprintf(token, "%s.%s", sign_input, sig_b64);
	}

	free(header_b64);
	free(payload_b64);
	free(sign_input);

	return token;
}

/**
 * @brief Verify an HS256 JWT token and return its payload.
 *
 * Splits the token into its three dot-separated components (header, payload,
 * signature), recomputes the HMAC-SHA256 signature over the signing input,
 * and compares it against the provided signature (constant-time not guaranteed
 * — uses strcmp). On success, the payload is base64url-decoded and parsed
 * into a cJSON object.
 *
 * @param c      The request context (used for HMAC operations).
 * @param token  The JWT string in the format `header.payload.signature`.
 * @param secret The HMAC-SHA256 verification secret.
 *
 * @return A newly allocated cJSON object representing the payload claims,
 *         or NULL if the token is malformed, the signature is invalid, or
 *         memory allocation fails.
 *
 * @note The caller owns the returned cJSON object and must free it with
 *       cJSON_Delete() when no longer needed.
 * @warning Signature comparison uses strcmp, which is NOT constant-time.
 *          This may be vulnerable to timing attacks in high-security
 *          environments.
 */
cJSON*
csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret)
{
	if (!token || !secret) {
		return NULL;
	}

	/* Locate the two dots that separate header, payload, and signature.
     JWT format: base64url(header).base64url(payload).base64url(signature) */
	const char* dot1 = strchr(token, '.');
	if (!dot1) {
		return NULL;
	}
	const char* dot2 = strchr(dot1 + 1, '.');
	if (!dot2) {
		return NULL;
	}

	size_t header_len = (size_t)(dot1 - token);
	size_t payload_len = (size_t)(dot2 - dot1 - 1);
	const char* sig_ptr = dot2 + 1;

	/* Step 1: Verify the HMAC-SHA256 signature.
     Recompute the signature over the signing input (header.payload)
     and compare it against the provided signature. A non-constant-time
     strcmp is used; consider memcmp + xor-constant-time for high-security
     deployments. */
	size_t sign_input_len = (size_t)(dot2 - token);
	uint8_t sig_actual[32];
	_csilk_hmac_sha256(c,
			   (const uint8_t*)secret,
			   strlen(secret),
			   (const uint8_t*)token,
			   sign_input_len,
			   sig_actual);

	char sig_expected_b64[45];
	csilk_base64url_encode(sig_actual, 32, sig_expected_b64);

	if (strcmp(sig_ptr, sig_expected_b64) != 0) {
		return NULL; /* Signature mismatch — token has been tampered with. */
	}

	/* Step 2: Base64url-decode the payload and parse it as JSON.
     The decoded JSON string is parsed with cJSON_Parse. */
	char* p_b64 = malloc(payload_len + 1);
	if (!p_b64) {
		return NULL;
	}
	memcpy(p_b64, dot1 + 1, payload_len);
	p_b64[payload_len] = '\0';

	uint8_t* p_json_str = malloc(payload_len + 1);
	if (!p_json_str) {
		free(p_b64);
		return NULL;
	}
	int p_decoded_len = csilk_base64url_decode(p_b64, p_json_str);
	free(p_b64);

	if (p_decoded_len < 0) {
		free(p_json_str);
		return NULL;
	}
	p_json_str[p_decoded_len] = '\0';

	cJSON* payload = cJSON_Parse((const char*)p_json_str);
	free(p_json_str);

	return payload;
}

/**
 * @brief JWT authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, verifies it via
 * csilk_jwt_verify(), and checks the "exp" claim if present. On success the
 * decoded payload is stored in the context under the key "jwt_payload" and
 * the next handler is called. On failure (missing header, invalid token, or
 * expired), a 401 Unauthorized response is sent.
 *
 * @param c      The request context.
 * @param secret The HMAC-SHA256 verification secret.
 *
 * @note The jwt_payload is stored with csilk_set() and is NOT automatically
 *       freed by the context cleanup. Downstream handlers should retrieve
 *       it with csilk_get() and call cJSON_Delete() when done, or register
 *       a cleanup callback.
 * @warning This middleware must be registered before any handler that
 *          accesses the jwt_payload via csilk_get(c, "jwt_payload").
 */
void
csilk_jwt_middleware(csilk_ctx_t* c, const char* secret)
{
	if (!c || !secret) {
		return;
	}

	const char* auth_header = csilk_get_header(c, "Authorization");
	if (!auth_header || strncmp(auth_header, "Bearer ", 7) != 0) {
		csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Bearer token required");
		csilk_abort(c);
		return;
	}

	const char* token = auth_header + 7;
	cJSON* payload = csilk_jwt_verify(c, token, secret);
	if (!payload) {
		csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Invalid or expired token");
		csilk_abort(c);
		return;
	}

	// Check expiration if 'exp' claim exists
	cJSON* exp = cJSON_GetObjectItemCaseSensitive(payload, "exp");
	if (cJSON_IsNumber(exp)) {
		if ((double)time(NULL) > exp->valuedouble) {
			cJSON_Delete(payload);
			csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Token expired");
			csilk_abort(c);
			return;
		}
	}

	/* Store decoded payload on context for downstream handlers.
     NOTE: csilk_ctx cleanup does NOT free cJSON objects. The downstream
     handler must call cJSON_Delete() on the payload retrieved via
     csilk_get(c, "jwt_payload") when it is no longer needed. */
	csilk_set(c, "jwt_payload", payload);

	csilk_next(c);
}
