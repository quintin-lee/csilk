/**
 * @file jwt.c
 * @brief JWT (JSON Web Token) generation and verification middleware.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"
#include "csilk/core/crypto.h"

/**
 * @brief Map a JWT algorithm to its RFC 7518 "alg" header value.
 */
static const char*
jwt_alg_str(csilk_jwt_alg_t alg)
{
    switch (alg) {
    case CSILK_JWT_RS256:
        return "RS256";
    case CSILK_JWT_ES256:
        return "ES256";
    default:
        return "HS256";
    }
}

/** @brief Build a JSON-formatted JWT header for the given algorithm
 * (caller must free the returned string).
 */
static char*
jwt_build_header(csilk_jwt_alg_t alg)
{
    const char* alg_str = jwt_alg_str(alg);
    char*       hdr = malloc(48);
    if (hdr) {
        snprintf(hdr, 48, "{\"alg\":\"%s\",\"typ\":\"JWT\"}", alg_str);
    }
    return hdr;
}

/**
 * @brief Constant-time string comparison to prevent timing attacks.
 *
 * Compares two byte sequences of known lengths without branching on
 * individual byte differences.
 */
static int
constant_time_compare(const uint8_t* a, const uint8_t* b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return (int)(diff != 0);
}

/**
 * @brief Internal: Generate a JWT token with a specified algorithm.
 */
static char*
jwt_generate_internal(
    csilk_ctx_t* c, cJSON* payload, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    if (!payload || !key) {
        CSILK_LOG_E("JWT: Generation failed: invalid arguments");
        return nullptr;
    }

    char* header_b64 = nullptr;
    char* payload_b64 = nullptr;
    char* token = nullptr;
    char* header_json = jwt_build_header(algorithm);
    if (!header_json) {
        return nullptr;
    }

    /* Step 1: Base64url-encode the JWT header. */
    size_t h_len = strlen(header_json);
    if (h_len > SIZE_MAX / 4 - 1) {
        free(header_json);
        return nullptr;
    }
    size_t h_b64_len = ((h_len + 2) / 3) * 4 + 1;
    header_b64 = malloc(h_b64_len);
    if (!header_b64) {
        free(header_json);
        return nullptr;
    }
    csilk_base64url_encode((const uint8_t*)header_json, h_len, header_b64);
    free(header_json);

    /* Step 2: Serialize payload to JSON, then base64url-encode. */
    char* payload_str = cJSON_PrintUnformatted(payload);
    if (!payload_str) {
        free(header_b64);
        return nullptr;
    }
    size_t p_len = strlen(payload_str);
    if (p_len > SIZE_MAX / 4 - 1) {
        free(header_b64);
        free(payload_str);
        return nullptr;
    }
    size_t p_b64_len = ((p_len + 2) / 3) * 4 + 1;
    payload_b64 = malloc(p_b64_len);
    if (!payload_b64) {
        free(header_b64);
        free(payload_str);
        return nullptr;
    }
    csilk_base64url_encode((const uint8_t*)payload_str, p_len, payload_b64);
    free(payload_str);

    /* Step 3: Build signing input: header.payload */
    size_t hb64_len = strlen(header_b64);
    size_t pb64_len = strlen(payload_b64);
    if (hb64_len > SIZE_MAX - 2 - pb64_len) {
        free(header_b64);
        free(payload_b64);
        return nullptr;
    }
    size_t sign_input_len = hb64_len + 1 + pb64_len + 1;
    char*  sign_input = malloc(sign_input_len);
    if (!sign_input) {
        free(header_b64);
        free(payload_b64);
        return nullptr;
    }
    snprintf(sign_input, sign_input_len, "%s.%s", header_b64, payload_b64);

    /* Step 4: Compute signature */
    char sig_b64[512];
    sig_b64[0] = '\0';

    if (algorithm == CSILK_JWT_HS256) {
        uint8_t sig[32];
        _csilk_hmac_sha256(
            c, (const uint8_t*)key, key_len, (const uint8_t*)sign_input, strlen(sign_input), sig);
        csilk_base64url_encode(sig, 32, sig_b64);
        explicit_bzero(sig, sizeof(sig));
    } else {
        size_t sig_len =
            (algorithm == CSILK_JWT_ES256) ? CSILK_ES256_SIGNATURE_SIZE : CSILK_RSA_SIGNATURE_SIZE;
        uint8_t sig[CSILK_RSA_SIGNATURE_SIZE];
        if (_csilk_jwt_sign(c,
                            key,
                            key_len,
                            (const uint8_t*)sign_input,
                            strlen(sign_input),
                            sig,
                            &sig_len,
                            algorithm) != 0) {
            CSILK_LOG_E("JWT: Signing failed for alg=%s", jwt_alg_str(algorithm));
            free(header_b64);
            free(payload_b64);
            free(sign_input);
            return nullptr;
        }
        csilk_base64url_encode(sig, sig_len, sig_b64);
        explicit_bzero(sig, sizeof(sig));
    }

    /* Step 5: Assemble final token */
    size_t si_len = strlen(sign_input);
    size_t sb_len = strlen(sig_b64);
    if (si_len > SIZE_MAX - 2 - sb_len) {
        free(header_b64);
        free(payload_b64);
        free(sign_input);
        explicit_bzero(sig_b64, sizeof(sig_b64));
        return nullptr;
    }
    token = malloc(si_len + 1 + sb_len + 1);
    if (token) {
        snprintf(token, si_len + 1 + sb_len + 1, "%s.%s", sign_input, sig_b64);
    }

    free(header_b64);
    free(payload_b64);
    free(sign_input);
    explicit_bzero(sig_b64, sizeof(sig_b64));
    return token;
}

char*
csilk_jwt_generate(csilk_ctx_t* c, cJSON* payload, const char* secret)
{
    return jwt_generate_internal(c, payload, secret, secret ? strlen(secret) : 0, CSILK_JWT_HS256);
}

char*
csilk_jwt_generate_ex(
    csilk_ctx_t* c, cJSON* payload, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    return jwt_generate_internal(c, payload, key, key_len, algorithm);
}

/**
 * @brief Internal: Verify a JWT token with a specified algorithm.
 */
static cJSON*
jwt_verify_internal(
    csilk_ctx_t* c, const char* token, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    if (!token || !key) {
        CSILK_LOG_E("JWT: Verification failed: invalid arguments");
        return nullptr;
    }

    /* Locate the two dots separating header, payload, signature. */
    const char* dot1 = strchr(token, '.');
    if (!dot1) {
        CSILK_LOG_W("JWT: Verification failed: missing first dot");
        return nullptr;
    }
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        CSILK_LOG_W("JWT: Verification failed: missing second dot");
        return nullptr;
    }

    size_t      payload_len = (size_t)(dot2 - dot1 - 1);
    const char* sig_ptr = dot2 + 1;
    size_t      sign_input_len = (size_t)(dot2 - token);
    int         sig_ok = 0;

    if (algorithm == CSILK_JWT_HS256) {
        /* HS256: HMAC-SHA256 + constant-time compare */
        uint8_t sig_actual[32];
        _csilk_hmac_sha256(
            c, (const uint8_t*)key, key_len, (const uint8_t*)token, sign_input_len, sig_actual);
        char sig_expected_b64[45];
        csilk_base64url_encode(sig_actual, 32, sig_expected_b64);
        explicit_bzero(sig_actual, sizeof(sig_actual));
        size_t sig_len = strlen(sig_ptr);
        sig_ok = (sig_len == strlen(sig_expected_b64)) &&
                 (constant_time_compare(
                      (const uint8_t*)sig_ptr, (const uint8_t*)sig_expected_b64, sig_len) == 0);
    } else {
        /* RS256/ES256: base64url-decode sig, then verify via cipher driver */
        uint8_t sig_decoded[CSILK_RSA_SIGNATURE_SIZE];
        int     dec_len = csilk_base64url_decode(sig_ptr, sig_decoded, CSILK_RSA_SIGNATURE_SIZE);
        if (dec_len > 0) {
            sig_ok = (_csilk_jwt_verify(c,
                                        key,
                                        key_len,
                                        (const uint8_t*)token,
                                        sign_input_len,
                                        sig_decoded,
                                        (size_t)dec_len,
                                        algorithm) == 0);
        }
        explicit_bzero(sig_decoded, sizeof(sig_decoded));
    }

    if (!sig_ok) {
        CSILK_LOG_W("JWT: Verification failed: signature mismatch");
        return nullptr;
    }

    /* Decode and parse the payload. */
    char* p_b64 = malloc(payload_len + 1);
    if (!p_b64) {
        return nullptr;
    }
    memcpy(p_b64, dot1 + 1, payload_len);
    p_b64[payload_len] = '\0';

    uint8_t* p_json_str = malloc(payload_len + 1);
    if (!p_json_str) {
        free(p_b64);
        return nullptr;
    }
    int p_decoded_len = csilk_base64url_decode(p_b64, p_json_str, payload_len + 1);
    free(p_b64);

    if (p_decoded_len < 0) {
        CSILK_LOG_W("JWT: base64url decode failed for payload");
        free(p_json_str);
        return nullptr;
    }
    p_json_str[p_decoded_len] = '\0';

    cJSON* payload = cJSON_Parse((const char*)p_json_str);
    free(p_json_str);
    return payload;
}

cJSON*
csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret)
{
    return jwt_verify_internal(c, token, secret, secret ? strlen(secret) : 0, CSILK_JWT_HS256);
}

cJSON*
csilk_jwt_verify_ex(
    csilk_ctx_t* c, const char* token, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    return jwt_verify_internal(c, token, key, key_len, algorithm);
}

/**
 * @brief JWT authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header and verifies it.
 * Supports HS256 (raw secret), RS256, and ES256 (PEM-encoded public key).
 *
 * @param c          The request context.
 * @param key        Verification key (raw string for HS256, PEM for RS256/ES256).
 * @param key_len    Key length in bytes.
 * @param algorithm  JWT algorithm (0 = HS256).
 */
void
csilk_jwt_middleware_ex(csilk_ctx_t* c, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    if (!c || !key) {
        CSILK_LOG_E("JWT: Middleware error: invalid arguments");
        return;
    }

    const char* auth_header = csilk_get_header(c, "Authorization");
    if (!auth_header || strncmp(auth_header, "Bearer ", 7) != 0) {
        CSILK_LOG_W("JWT: Middleware: missing or invalid Authorization header");
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Bearer token required");
        csilk_abort(c);
        return;
    }

    const char* token = auth_header + 7;
    cJSON*      payload = jwt_verify_internal(c, token, key, key_len, algorithm);
    if (!payload) {
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Invalid or expired token");
        csilk_abort(c);
        return;
    }

    /* Check expiration if 'exp' claim exists */
    cJSON* exp = cJSON_GetObjectItemCaseSensitive(payload, "exp");
    if (cJSON_IsNumber(exp)) {
        if ((double)time(nullptr) > exp->valuedouble) {
            cJSON_Delete(payload);
            csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Token expired");
            csilk_abort(c);
            return;
        }
    }

    csilk_set(c, "jwt_payload", payload);
    csilk_next(c);
}

void
csilk_jwt_middleware(csilk_ctx_t* c, const char* secret)
{
    csilk_jwt_middleware_ex(c, secret, secret ? strlen(secret) : 0, CSILK_JWT_HS256);
}

char*
csilk_ctx_get_jwt_payload_json(csilk_ctx_t* c)
{
    if (!c) {
        return nullptr;
    }
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    if (!payload) {
        return nullptr;
    }
    char* json_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    csilk_set(c, "jwt_payload", nullptr);
    return json_str;
}

void
csilk_ctx_cleanup_jwt_payload(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    if (payload) {
        cJSON_Delete(payload);
        csilk_set(c, "jwt_payload", nullptr);
    }
}

char*
csilk_jwt_generate_json(csilk_ctx_t* c, const char* payload_json, const char* secret)
{
    if (!payload_json || !secret) {
        return nullptr;
    }
    cJSON* payload = cJSON_Parse(payload_json);
    if (!payload) {
        return nullptr;
    }
    char* token = csilk_jwt_generate(c, payload, secret);
    cJSON_Delete(payload);
    return token;
}
