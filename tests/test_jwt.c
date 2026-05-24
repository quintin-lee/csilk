/**
 * @file test_jwt.c
 * @brief Unit tests for JWT generation and verification.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

void test_jwt_core() {
  printf("Testing JWT core generation and verification...\n");

  cJSON* payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sub", "1234567890");
  cJSON_AddStringToObject(payload, "name", "John Doe");
  cJSON_AddNumberToObject(payload, "iat", 1516239022);

  const char* secret = "secret";

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));

  char* token = csilk_jwt_generate(&c, payload, secret);
  assert(token != NULL);
  printf("Generated Token: %s\n", token);

  cJSON* verified_payload = csilk_jwt_verify(&c, token, secret);
  assert(verified_payload != NULL);

  assert(cJSON_HasObjectItem(verified_payload, "sub"));
  assert(strcmp(cJSON_GetObjectItem(verified_payload, "sub")->valuestring,
                "1234567890") == 0);

  // Test invalid secret
  cJSON* invalid_payload = csilk_jwt_verify(&c, token, "wrong_secret");
  assert(invalid_payload == NULL);

  // Test tampered token
  token[strlen(token) - 1] ^= 0xFF;  // Flip a bit in signature
  cJSON* tampered_payload = csilk_jwt_verify(&c, token, secret);
  assert(tampered_payload == NULL);

  free(token);
  cJSON_Delete(payload);
  cJSON_Delete(verified_payload);
}

void dummy_handler(csilk_ctx_t* c) { (void)c; }

void test_jwt_middleware() {
  printf("Testing JWT middleware...\n");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  // Setup handlers for csilk_next
  csilk_handler_t handlers[] = {dummy_handler, dummy_handler, NULL};
  c.handlers = handlers;
  c.handler_index = -1;

  const char* secret = "supersecret";
  cJSON* payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "user", "admin");
  char* token = csilk_jwt_generate(&c, payload, secret);

  char auth_header[512];
  sprintf(auth_header, "Bearer %s", token);

  // 1. Success case
  csilk_set_request_header(&c, "Authorization", auth_header);
  csilk_jwt_middleware(&c, secret);
  assert(csilk_is_aborted(&c) == 0);

  cJSON* stored_payload = (cJSON*)csilk_get(&c, "jwt_payload");
  assert(stored_payload != NULL);
  assert(strcmp(cJSON_GetObjectItem(stored_payload, "user")->valuestring,
                "admin") == 0);
  cJSON_Delete(stored_payload);

  // 2. Failure case: missing header
  csilk_ctx_cleanup(&c);
  c.arena = csilk_arena_new(1024);
  c.handlers = handlers;
  c.handler_index = -1;
  csilk_jwt_middleware(&c, secret);
  assert(csilk_is_aborted(&c) == 1);
  assert(csilk_get_status(&c) == CSILK_STATUS_UNAUTHORIZED);

  // 3. Failure case: invalid token
  csilk_ctx_cleanup(&c);
  c.arena = csilk_arena_new(1024);
  c.handlers = handlers;
  c.handler_index = -1;
  csilk_set_request_header(&c, "Authorization", "Bearer invalid.token.here");
  csilk_jwt_middleware(&c, secret);
  assert(csilk_is_aborted(&c) == 1);
  assert(csilk_get_status(&c) == CSILK_STATUS_UNAUTHORIZED);

  free(token);
  cJSON_Delete(payload);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
}

void test_jwt_expiration() {
  printf("Testing JWT expiration...\n");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);
  csilk_handler_t handlers[] = {dummy_handler, NULL};
  c.handlers = handlers;
  c.handler_index = -1;

  const char* secret = "secret";
  cJSON* payload = cJSON_CreateObject();
  cJSON_AddNumberToObject(payload, "exp",
                          (double)time(NULL) - 10);  // Expired 10s ago

  char* token = csilk_jwt_generate(&c, payload, secret);
  char auth_header[512];
  sprintf(auth_header, "Bearer %s", token);

  csilk_set_request_header(&c, "Authorization", auth_header);
  csilk_jwt_middleware(&c, secret);

  assert(csilk_is_aborted(&c) == 1);
  assert(csilk_get_status(&c) == CSILK_STATUS_UNAUTHORIZED);

  free(token);
  cJSON_Delete(payload);
  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
}

int main() {
  test_jwt_core();
  test_jwt_middleware();
  test_jwt_expiration();
  printf("All JWT tests passed!\n");
  return 0;
}
