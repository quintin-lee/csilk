/**
 * @file jwt.c
 * @brief JWT (JSON Web Token) generation and verification middleware.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Header for HS256 JWT. */
static const char* JWT_HEADER = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

char* csilk_jwt_generate(cJSON* payload, const char* secret) {
  if (!payload || !secret) return NULL;

  char* header_b64 = NULL;
  char* payload_b64 = NULL;
  char* token = NULL;

  // 1. Encode header
  size_t h_len = strlen(JWT_HEADER);
  size_t h_b64_len = ((h_len + 2) / 3) * 4 + 1;
  header_b64 = malloc(h_b64_len);
  if (!header_b64) return NULL;
  csilk_base64url_encode((const uint8_t*)JWT_HEADER, h_len, header_b64);

  // 2. Encode payload
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

  // 3. Create signing input
  size_t sign_input_len = strlen(header_b64) + 1 + strlen(payload_b64) + 1;
  char* sign_input = malloc(sign_input_len);
  if (!sign_input) {
    free(header_b64);
    free(payload_b64);
    return NULL;
  }
  sprintf(sign_input, "%s.%s", header_b64, payload_b64);

  // 4. Sign
  uint8_t sig[32];
  csilk_hmac_sha256((const uint8_t*)secret, strlen(secret),
                    (const uint8_t*)sign_input, strlen(sign_input), sig);

  char sig_b64[45];  // 32 bytes -> 43 chars + padding + null
  csilk_base64url_encode(sig, 32, sig_b64);

  // 5. Build final token
  token = malloc(strlen(sign_input) + 1 + strlen(sig_b64) + 1);
  if (token) {
    sprintf(token, "%s.%s", sign_input, sig_b64);
  }

  free(header_b64);
  free(payload_b64);
  free(sign_input);

  return token;
}

cJSON* csilk_jwt_verify(const char* token, const char* secret) {
  if (!token || !secret) return NULL;

  const char* dot1 = strchr(token, '.');
  if (!dot1) return NULL;
  const char* dot2 = strchr(dot1 + 1, '.');
  if (!dot2) return NULL;

  size_t header_len = (size_t)(dot1 - token);
  size_t payload_len = (size_t)(dot2 - dot1 - 1);
  const char* sig_ptr = dot2 + 1;

  // 1. Verify signature
  size_t sign_input_len = (size_t)(dot2 - token);
  uint8_t sig_actual[32];
  csilk_hmac_sha256((const uint8_t*)secret, strlen(secret),
                    (const uint8_t*)token, sign_input_len, sig_actual);

  char sig_expected_b64[45];
  csilk_base64url_encode(sig_actual, 32, sig_expected_b64);

  if (strcmp(sig_ptr, sig_expected_b64) != 0) {
    return NULL;  // Invalid signature
  }

  // 2. Parse payload
  char* p_b64 = malloc(payload_len + 1);
  if (!p_b64) return NULL;
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

void csilk_jwt_middleware(csilk_ctx_t* c, const char* secret) {
  if (!c || !secret) return;

  const char* auth_header = csilk_get_header(c, "Authorization");
  if (!auth_header || strncmp(auth_header, "Bearer ", 7) != 0) {
    csilk_json_error(c, CSILK_STATUS_UNAUTHORIZED, "Bearer token required");
    csilk_abort(c);
    return;
  }

  const char* token = auth_header + 7;
  cJSON* payload = csilk_jwt_verify(token, secret);
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

  csilk_set(c, "jwt_payload", payload);
  // Note: csilk_ctx_cleanup won't free this cJSON object automatically.
  // We should ideally have a way to register cleanup for storage items.
  // For now, let's document that user must handle it OR we add a clear callback
  // but that's complex.
  // Actually, csilk_set values are usually managed by user or arena.
  // But cJSON* needs cJSON_Delete.

  csilk_next(c);
}
