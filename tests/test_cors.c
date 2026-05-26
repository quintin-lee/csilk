#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

static const char* get_response_header(csilk_ctx_t* ctx, const char* key) {
  for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
    csilk_header_t* h = ctx->response.headers.buckets[i];
    while (h) {
      if (strcasecmp(h->key, key) == 0) return h->value;
      h = h->next;
    }
  }
  return NULL;
}

static void test_cors_basic() {
  printf("Testing CORS middleware basic...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);

  csilk_cors_config_t config = {.allow_origin = "*",
                                .allow_methods = "GET,POST",
                                .allow_headers = "Content-Type",
                                .allow_credentials = 0,
                                .max_age = 3600};

  csilk_handler_t handlers[] = {NULL};
  ctx.handlers = handlers;
  ctx.handler_index = -1;

  csilk_cors_middleware(&ctx, &config);

  const char* origin = get_response_header(&ctx, "Access-Control-Allow-Origin");
  assert(origin != NULL);
  assert(strcmp(origin, "*") == 0);

  const char* methods =
      get_response_header(&ctx, "Access-Control-Allow-Methods");
  assert(methods != NULL);
  assert(strcmp(methods, "GET,POST") == 0);

  const char* max_age = get_response_header(&ctx, "Access-Control-Max-Age");
  assert(max_age != NULL);
  assert(strcmp(max_age, "3600") == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("CORS basic test passed!\n");
}

static void test_cors_null_config() {
  printf("Testing CORS with NULL config...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  csilk_handler_t handlers[] = {NULL};
  ctx.handlers = handlers;
  ctx.handler_index = -1;

  csilk_cors_middleware(&ctx, NULL);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("CORS null config test passed!\n");
}

static void test_cors_credentials() {
  printf("Testing CORS with credentials...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  ctx.handler_index = -1;

  csilk_cors_config_t config = {.allow_origin = "https://example.com",
                                .allow_methods = "GET",
                                .allow_headers = "*",
                                .allow_credentials = 1,
                                .max_age = 0};

  csilk_handler_t handlers[] = {NULL};
  ctx.handlers = handlers;
  ctx.handler_index = -1;

  csilk_cors_middleware(&ctx, &config);

  const char* creds =
      get_response_header(&ctx, "Access-Control-Allow-Credentials");
  assert(creds != NULL);
  assert(strcmp(creds, "true") == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("CORS credentials test passed!\n");
}

int main() {
  test_cors_basic();
  test_cors_null_config();
  test_cors_credentials();
  printf("test_cors: ALL PASSED\n");
  return 0;
}
