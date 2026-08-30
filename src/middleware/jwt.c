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

static void
_csilk_jwt_json_free(void* value)
{
    csilk_json_free((csilk_json_t*)value);
}
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"
#include <openssl/crypto.h>

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
    case CSILK_JWT_HS256:
        return "HS256";
    default:
        return "unknown algorithm";
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
jwt_generate_internal(csilk_ctx_t*    c,
                      csilk_json_t*   payload,
                      const char*     key,
                      size_t          key_len,
                      csilk_jwt_alg_t algorithm)
{
    if (!payload || !key) {
        CSILK_LOG_E("JWT: Generation failed: invalid arguments");
        return NULL;
    }

    char* header_b64 = NULL;
    char* payload_b64 = NULL;
    char* token = NULL;
    char* header_json = jwt_build_header(algorithm);
    if (!header_json) {
        return NULL;
    }

    /* Step 1: Base64url-encode the JWT header. */
    size_t h_len = strlen(header_json);
    if (h_len > SIZE_MAX / 4 - 1) {
        free(header_json);
        return NULL;
    }
    size_t h_b64_len = ((h_len + 2) / 3) * 4 + 1;
    header_b64 = malloc(h_b64_len);
    if (!header_b64) {
        free(header_json);
        return NULL;
    }
    csilk_base64url_encode((const uint8_t*)header_json, h_len, header_b64);
    free(header_json);

    /* Step 2: Serialize payload to JSON, then base64url-encode. */
    char* payload_str = csilk_json_serialize(payload, NULL);
    if (!payload_str) {
        free(header_b64);
        return NULL;
    }
    size_t p_len = strlen(payload_str);
    if (p_len > SIZE_MAX / 4 - 1) {
        free(header_b64);
        free(payload_str);
        return NULL;
    }
    size_t p_b64_len = ((p_len + 2) / 3) * 4 + 1;
    payload_b64 = malloc(p_b64_len);
    if (!payload_b64) {
        free(header_b64);
        free(payload_str);
        return NULL;
    }
    csilk_base64url_encode((const uint8_t*)payload_str, p_len, payload_b64);
    free(payload_str);

    /* Step 3: Build signing input: header.payload */
    size_t hb64_len = strlen(header_b64);
    size_t pb64_len = strlen(payload_b64);
    if (hb64_len > SIZE_MAX - 2 - pb64_len) {
        free(header_b64);
        free(payload_b64);
        return NULL;
    }
    size_t sign_input_len = hb64_len + 1 + pb64_len + 1;
    char*  sign_input = malloc(sign_input_len);
    if (!sign_input) {
        free(header_b64);
        free(payload_b64);
        return NULL;
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
            return NULL;
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
        return NULL;
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

/**
 * @brief Generate a signed JWT using the HS256 (HMAC-SHA256) algorithm.
 *
 * Convenience wrapper around jwt_generate_internal() that encodes the supplied
 * payload with a raw secret string as the HMAC key.
 *
 * @param c        The request context (used for crypto operations).
 * @param payload  JSON object carrying the JWT claims. Must not be NULL.
 * @param secret   Null-terminated HMAC secret. May be NULL (produces NULL token).
 *
 * @return A newly allocated null-terminated JWT string (caller frees), or NULL
 *         on error or invalid arguments.
 */
char*
csilk_jwt_generate(csilk_ctx_t* c, csilk_json_t* payload, const char* secret)
{
    return jwt_generate_internal(c, payload, secret, secret ? strlen(secret) : 0, CSILK_JWT_HS256);
}

/**
 * @brief Generate a signed JWT using the specified algorithm and key.
 *
 * Supports HS256 (raw secret), RS256, and ES256 (key material). The signature
 * is computed via the framework's cipher driver.
 *
 * @param c          The request context (used for crypto operations).
 * @param payload    JSON object carrying the JWT claims. Must not be NULL.
 * @param key        Key bytes (raw secret for HS256, PEM for RS256/ES256).
 * @param key_len    Length of @p key in bytes.
 * @param algorithm  Algorithm selector (CSILK_JWT_HS256, CSILK_JWT_RS256,
 *                   CSILK_JWT_ES256).
 *
 * @return A newly allocated null-terminated JWT string (caller frees), or NULL
 *         on error or invalid arguments.
 */
char*
csilk_jwt_generate_ex(csilk_ctx_t*    c,
                      csilk_json_t*   payload,
                      const char*     key,
                      size_t          key_len,
                      csilk_jwt_alg_t algorithm)
{
    return jwt_generate_internal(c, payload, key, key_len, algorithm);
}

/**
 * @brief Internal: Verify a JWT token with full validation options.
 */
static csilk_json_t*
jwt_verify_internal(csilk_ctx_t*               c,
                    const char*                token,
                    const char*                key,
                    size_t                     key_len,
                    const csilk_jwt_options_t* options)
{
    if (!token || !key) {
        CSILK_LOG_E("JWT: Verification failed: invalid arguments");
        return NULL;
    }

    /* Reject trivially weak HMAC keys (< 16 bytes) - brute-force attack surface. */
    if (key_len < 16) {
        CSILK_LOG_E("JWT: Verification failed: HMAC key too short (min 16 bytes, got %zu)",
                    key_len);
        return NULL;
    }

    csilk_jwt_alg_t algorithm = options ? options->algorithm : CSILK_JWT_HS256;
    uint32_t        flags = options ? options->flags : CSILK_JWT_NONE;
    int64_t         leeway = options ? options->leeway_sec : 0;

    /* Locate the two dots separating header, payload, signature. */
    const char* dot1 = strchr(token, '.');
    if (!dot1) {
        CSILK_LOG_W("JWT: Verification failed: missing first dot");
        return NULL;
    }
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        CSILK_LOG_W("JWT: Verification failed: missing second dot");
        return NULL;
    }

    /* --- Algorithm confusion guard (CWE-327) ---
     * Decode the JWT header and verify that the declared "alg" matches the
     * server-configured expected algorithm.  This prevents attacks where an
     * attacker forges a token with alg=HS256 using an RS256 public key as
     * the HMAC secret. */
    {
        size_t hdr_b64_len = (size_t)(dot1 - token);
        char*  hdr_b64 = malloc(hdr_b64_len + 1);
        if (!hdr_b64) {
            return NULL;
        }
        memcpy(hdr_b64, token, hdr_b64_len);
        hdr_b64[hdr_b64_len] = '\0';

        uint8_t* hdr_json = malloc(hdr_b64_len + 1);
        if (!hdr_json) {
            free(hdr_b64);
            return NULL;
        }
        int hdr_decoded_len = csilk_base64url_decode(hdr_b64, hdr_json, hdr_b64_len + 1);
        free(hdr_b64);
        if (hdr_decoded_len < 0) {
            CSILK_LOG_W("JWT: Verification failed: header base64url decode error");
            free(hdr_json);
            return NULL;
        }
        hdr_json[hdr_decoded_len] = '\0';

        csilk_json_t* hdr_obj = csilk_json_parse((const char*)hdr_json);
        free(hdr_json);
        if (!hdr_obj) {
            CSILK_LOG_W("JWT: Verification failed: header JSON parse error");
            return NULL;
        }
        csilk_json_t* alg_val = csilk_json_get(hdr_obj, "alg");
        const char*   alg_str_in_token = alg_val ? csilk_json_string_value(alg_val) : NULL;
        const char*   expected_alg = jwt_alg_str(algorithm);
        int           alg_match = 0;
        if (alg_str_in_token && expected_alg) {
            alg_match = (strcmp(alg_str_in_token, expected_alg) == 0);
        }
        csilk_json_free(hdr_obj);

        if (!alg_match) {
            CSILK_LOG_W("JWT: Verification failed: algorithm mismatch "
                        "(header: '%s', expected: '%s')",
                        alg_str_in_token ? alg_str_in_token : "null",
                        expected_alg ? expected_alg : "null");
            return NULL;
        }
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
        /* Fixed-length comparison with OpenSSL CRYPTO_memcmp:
         * - Compares exactly 43 bytes (32 bytes base64url-encoded, no padding)
         * - Compiler-resilient against timing optimization
         * - No OOB risk since length is known at compile time */
        size_t expected_len = strlen(sig_expected_b64);
        size_t sig_len = strlen(sig_ptr);
        int    len_mismatch = (sig_len != expected_len);
        /* Always run CRYPTO_memcmp to prevent timing side-channel:
         * even if lengths differ, we compare to avoid leaking info */
        int cmp_result = len_mismatch ? 1
                                      : CRYPTO_memcmp((const uint8_t*)sig_ptr,
                                                      (const uint8_t*)sig_expected_b64,
                                                      expected_len);
        sig_ok = (cmp_result == 0);
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
        return NULL;
    }

    /* Decode and parse the payload. */
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
    int p_decoded_len = csilk_base64url_decode(p_b64, p_json_str, payload_len + 1);
    free(p_b64);

    if (p_decoded_len < 0) {
        CSILK_LOG_W("JWT: base64url decode failed for payload");
        free(p_json_str);
        return NULL;
    }
    p_json_str[p_decoded_len] = '\0';

    csilk_json_t* payload = csilk_json_parse((const char*)p_json_str);
    free(p_json_str);
    if (!payload) {
        CSILK_LOG_W("JWT: Failed to parse JSON payload");
        return NULL;
    }

    time_t now = time(NULL);

    /* Validate 'exp' claim */
    csilk_json_t* exp = csilk_json_get(payload, "exp");
    if (exp) {
        if (!csilk_json_is_number(exp)) {
            CSILK_LOG_W("JWT: 'exp' claim is not a valid number");
            csilk_json_free(payload);
            return NULL;
        }
        double exp_val = csilk_json_number_value(exp);
        if ((double)now - (double)leeway > exp_val) {
            CSILK_LOG_W("JWT: Token expired");
            csilk_json_free(payload);
            return NULL;
        }
    } else if (flags & CSILK_JWT_REQUIRE_EXP) {
        CSILK_LOG_W("JWT: Missing required 'exp' claim");
        csilk_json_free(payload);
        return NULL;
    }

    /* Validate 'nbf' claim */
    csilk_json_t* nbf = csilk_json_get(payload, "nbf");
    if (nbf) {
        if (!csilk_json_is_number(nbf)) {
            CSILK_LOG_W("JWT: 'nbf' claim is not a valid number");
            csilk_json_free(payload);
            return NULL;
        }
        double nbf_val = csilk_json_number_value(nbf);
        if ((double)now + (double)leeway < nbf_val) {
            CSILK_LOG_W("JWT: Token not yet valid (nbf)");
            csilk_json_free(payload);
            return NULL;
        }
    } else if (flags & CSILK_JWT_REQUIRE_NBF) {
        CSILK_LOG_W("JWT: Missing required 'nbf' claim");
        csilk_json_free(payload);
        return NULL;
    }

    /* Validate 'iat' claim */
    csilk_json_t* iat = csilk_json_get(payload, "iat");
    if (iat) {
        if (!csilk_json_is_number(iat)) {
            CSILK_LOG_W("JWT: 'iat' claim is not a valid number");
            csilk_json_free(payload);
            return NULL;
        }
    } else if (flags & CSILK_JWT_REQUIRE_IAT) {
        CSILK_LOG_W("JWT: Missing required 'iat' claim");
        csilk_json_free(payload);
        return NULL;
    }

    return payload;
}

/**
 * @brief Verify a JWT using the HS256 (HMAC-SHA256) algorithm.
 *
 * Convenience wrapper around csilk_jwt_verify_options() that uses a raw secret
 * string as the HMAC key.
 *
 * @param c        The request context (used for crypto operations).
 * @param token    Null-terminated JWT string. Must not be NULL.
 * @param secret   Null-terminated HMAC secret. May be NULL (fails verification).
 *
 * @return The decoded claims as a csilk_json_t on success (caller frees via
 *         csilk_json_free()), or NULL if the token is malformed or signature
 *         verification fails.
 */
csilk_json_t*
csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret)
{
    csilk_jwt_options_t opts = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_NONE,
        .leeway_sec = 0,
    };
    return jwt_verify_internal(c, token, secret, secret ? strlen(secret) : 0, &opts);
}

/**
 * @brief Verify a JWT using the specified algorithm and key.
 *
 * Supports HS256 (raw secret), RS256, and ES256 (PEM key). On success the
 * signature is validated before the payload is base64url-decoded and parsed.
 *
 * @param c          The request context (used for crypto operations).
 * @param token      Null-terminated JWT string. Must not be NULL.
 * @param key        Key bytes (raw secret for HS256, PEM for RS256/ES256).
 * @param key_len    Length of @p key in bytes.
 * @param algorithm  Algorithm selector (CSILK_JWT_HS256, CSILK_JWT_RS256,
 *                   CSILK_JWT_ES256).
 *
 * @return The decoded claims as a csilk_json_t on success (caller frees), or
 *         NULL if the token is malformed or verification fails.
 */
csilk_json_t*
csilk_jwt_verify_ex(
    csilk_ctx_t* c, const char* token, const char* key, size_t key_len, csilk_jwt_alg_t algorithm)
{
    csilk_jwt_options_t opts = {
        .algorithm = algorithm,
        .flags = CSILK_JWT_NONE,
        .leeway_sec = 0,
    };
    return jwt_verify_internal(c, token, key, key_len, &opts);
}

/**
 * @brief Verify a JWT with configurable validation options (algorithm, require_exp policy, leeway).
 *
 * @param c         The request context (used for crypto operations).
 * @param token     Null-terminated JWT string. Must not be NULL.
 * @param key       Key bytes (raw secret for HS256, PEM for RS256/ES256).
 * @param key_len   Length of @p key in bytes.
 * @param options   Validation options struct (may be NULL for defaults).
 *
 * @return The decoded claims as a csilk_json_t on success (caller frees), or
 *         NULL if validation fails.
 */
csilk_json_t*
csilk_jwt_verify_options(csilk_ctx_t*               c,
                         const char*                token,
                         const char*                key,
                         size_t                     key_len,
                         const csilk_jwt_options_t* options)
{
    return jwt_verify_internal(c, token, key, key_len, options);
}

/**
 * @brief JWT authentication middleware with configurable validation policy (e.g. CSILK_JWT_REQUIRE_EXP).
 *
 * Extracts the Bearer token from the Authorization header and validates it against
 * the configured options.
 *
 * @param c          The request context.
 * @param key        Verification key (raw string for HS256, PEM for RS256/ES256).
 * @param key_len    Key length in bytes.
 * @param options    JWT validation options (algorithm, flags, leeway). May be NULL.
 */
void
csilk_jwt_middleware_options(csilk_ctx_t*               c,
                             const char*                key,
                             size_t                     key_len,
                             const csilk_jwt_options_t* options)
{
    if (!c || !key) {
        CSILK_LOG_E("JWT: Middleware error: invalid arguments");
        if (c) {
            csilk_json_error(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "JWT configuration error");
            csilk_abort(c);
        }
        return;
    }

    const char* auth_header = csilk_get_header(c, "Authorization");
    if (!auth_header || strncmp(auth_header, "Bearer ", 7) != 0) {
        CSILK_LOG_W("JWT: Middleware: missing or invalid Authorization header");
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Bearer token required");
        csilk_abort(c);
        return;
    }

    const char*   token = auth_header + 7;
    csilk_json_t* payload = jwt_verify_internal(c, token, key, key_len, options);
    if (!payload) {
        csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Invalid or expired token");
        csilk_abort(c);
        return;
    }

    csilk_set_ex(c, "jwt_payload", payload, _csilk_jwt_json_free);
    csilk_next(c);
}

/**
 * @brief JWT authentication middleware with explicit algorithm parameter.
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
    csilk_jwt_options_t opts = {
        .algorithm = algorithm,
        .flags = CSILK_JWT_NONE,
        .leeway_sec = 0,
    };
    csilk_jwt_middleware_options(c, key, key_len, &opts);
}

/**
 * @brief JWT authentication middleware using HS256 (HMAC-SHA256).
 *
 * Convenience wrapper around csilk_jwt_middleware_ex() that strips the
 * "Bearer " prefix from the Authorization header, verifies the token with the
 * raw secret, and (on success) stores the claims under "jwt_payload" and calls
 * csilk_next(). A 401 is returned if the header is missing or the token is
 * invalid/expired.
 *
 * @param c        The request context.
 * @param secret   Null-terminated HMAC secret. If NULL the function logs an
 *                 error and returns without aborting.
 */
void
csilk_jwt_middleware(csilk_ctx_t* c, const char* secret)
{
    csilk_jwt_middleware_ex(c, secret, secret ? strlen(secret) : 0, CSILK_JWT_HS256);
}

/**
 * @brief Serialize and consume the stored JWT payload as a JSON string.
 *
 * Retrieves the "jwt_payload" claim object set by the JWT middleware, marshals
 * it to a JSON string, frees the stored object, and clears the context entry.
 *
 * @param c  The request context. Must not be NULL.
 *
 * @return A newly allocated JSON string (caller frees with free()), or NULL if
 *         no payload is present or @p c is NULL.
 */
char*
csilk_ctx_get_jwt_payload_json(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    csilk_json_t* payload = (csilk_json_t*)csilk_get(c, "jwt_payload");
    if (!payload) {
        return NULL;
    }
    char* json_str = csilk_json_serialize(payload, NULL);
    csilk_set(c, "jwt_payload", NULL);
    return json_str;
}

/**
 * @brief Free the stored JWT payload without serializing it.
 *
 * Frees the "jwt_payload" claim object set by the JWT middleware and clears the
 * context entry. Safe to call when no payload is present.
 *
 * @param c  The request context. Must not be NULL.
 */
void
csilk_ctx_cleanup_jwt_payload(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    csilk_set(c, "jwt_payload", NULL);
}

/**
 * @brief Generate a JWT from a JSON string payload using HS256.
 *
 * Parses the supplied JSON claim string into a csilk_json_t, then delegates to
 * csilk_jwt_generate() with the HS256 algorithm and the given secret.
 *
 * @param c              The request context (used for crypto operations).
 * @param payload_json   Null-terminated JSON string of claims. Must not be NULL.
 * @param secret         Null-terminated HMAC secret. Must not be NULL.
 *
 * @return A newly allocated null-terminated JWT string (caller frees), or NULL
 *         if the JSON is invalid or arguments are NULL.
 */
char*
csilk_jwt_generate_json(csilk_ctx_t* c, const char* payload_json, const char* secret)
{
    if (!payload_json || !secret) {
        return NULL;
    }
    csilk_json_t* payload = csilk_json_parse(payload_json);
    if (!payload) {
        return NULL;
    }
    char* token = csilk_jwt_generate(c, payload, secret);
    csilk_json_free(payload);
    return token;
}
