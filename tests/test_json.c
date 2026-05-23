#include "csilk_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

void test_bind_json() {
  printf("Testing csilk_bind_json...\n");
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.request.body = "{\"key\": \"value\"}";

  cJSON* json = csilk_bind_json(&ctx);
  assert(json != NULL);

  cJSON* val = cJSON_GetObjectItem(json, "key");
  assert(val != NULL);
  assert(strcmp(val->valuestring, "value") == 0);

  cJSON_Delete(json);
  printf("csilk_bind_json test passed.\n");
}

void test_json_response() {
  printf("Testing csilk_json...\n");
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "status", "ok");

  csilk_json(&ctx, 200, json);

  assert(ctx.response.status == 200);
  assert(ctx.response.body != NULL);
  // Note: csilk_set_header is not implemented for checking, but we rely on
  // csilk_json implementation The body should be "{"status":"ok"}"
  assert(strstr(ctx.response.body, "\"status\":\"ok\"") != NULL);

  csilk_ctx_cleanup(&ctx);
  printf("csilk_json test passed.\n");
}

void test_bind_json_err() {
  printf("Testing csilk_bind_json_err...\n");

  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  const char* err = NULL;
  cJSON* json = csilk_bind_json_err(NULL, &err);
  assert(json == NULL);
  assert(err != NULL);

  json = csilk_bind_json_err(&ctx, &err);
  assert(json == NULL);
  assert(err != NULL);
  assert(strcmp(err, "No request body") == 0);

  ctx.request.body = "invalid json {{{";
  json = csilk_bind_json_err(&ctx, &err);
  assert(json == NULL);
  assert(err != NULL);

  ctx.request.body = "{\"valid\": true}";
  json = csilk_bind_json_err(&ctx, &err);
  assert(json != NULL);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "valid")));
  cJSON_Delete(json);

  printf("csilk_bind_json_err passed!\n");
}

void test_json_error() {
  printf("Testing csilk_json_error...\n");

  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  csilk_json_error(&ctx, 404, "Not found");
  assert(ctx.response.status == 404);
  assert(ctx.response.body != NULL);
  assert(strstr(ctx.response.body, "\"error\"") != NULL);
  assert(strstr(ctx.response.body, "Not found") != NULL);
  csilk_ctx_cleanup(&ctx);

  memset(&ctx, 0, sizeof(ctx));
  csilk_json_error(&ctx, 500, NULL);
  assert(ctx.response.status == 500);
  assert(strstr(ctx.response.body, "Unknown error") != NULL);
  csilk_ctx_cleanup(&ctx);

  csilk_json_error(NULL, 400, "should not crash");
  printf("csilk_json_error passed!\n");
}

int main() {
  test_bind_json();
  test_json_response();
  test_bind_json_err();
  test_json_error();
  printf("All JSON tests passed!\n");
  return 0;
}
