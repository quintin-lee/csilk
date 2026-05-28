#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static int part_count = 0;
static char last_name[128];
static char last_filename[256];
static size_t last_data_len = 0;

static void reset_counters() {
  part_count = 0;
  last_name[0] = '\0';
  last_filename[0] = '\0';
  last_data_len = 0;
}

static void collect_part(csilk_multipart_part_t* part) {
  part_count++;
  strncpy(last_name, part->name, sizeof(last_name) - 1);
  strncpy(last_filename, part->filename, sizeof(last_filename) - 1);
  last_data_len = part->data_len;
}

static void test_multipart_simple() {
  printf("Testing multipart parse simple...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(4096);

  const char* body =
      "--boundary123\r\n"
      "Content-Disposition: form-data; name=\"field1\"\r\n"
      "\r\n"
      "value1\r\n"
      "--boundary123--\r\n";
  size_t body_len = strlen(body);

  ctx.request.body = malloc(body_len + 1);
  memcpy(ctx.request.body, body, body_len + 1);
  ctx.request.body_len = body_len;

  /* Set Content-Type directly via csilk_set_request_header */
  csilk_set_request_header(&ctx, "Content-Type",
                           "multipart/form-data; boundary=boundary123");

  /* Verify the header is set */
  const char* ct = csilk_get_header(&ctx, "Content-Type");
  assert(ct != NULL);
  assert(strstr(ct, "boundary=boundary123") != NULL);

  reset_counters();
  csilk_multipart_parse(&ctx, collect_part);
  assert(part_count == 1);
  assert(strcmp(last_name, "field1") == 0);
  assert(last_data_len == 6);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("Multipart simple test passed!\n");
}

static void test_multipart_file() {
  printf("Testing multipart parse with file...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(4096);

  const char* body =
      "--boundary123\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "Hello, World!\r\n"
      "--boundary123--\r\n";
  size_t body_len = strlen(body);

  ctx.request.body = malloc(body_len + 1);
  memcpy(ctx.request.body, body, body_len + 1);
  ctx.request.body_len = body_len;

  csilk_set_request_header(&ctx, "Content-Type",
                           "multipart/form-data; boundary=boundary123");

  reset_counters();
  csilk_multipart_parse(&ctx, collect_part);
  assert(part_count == 1);
  assert(strcmp(last_name, "file") == 0);
  assert(strcmp(last_filename, "test.txt") == 0);
  assert(last_data_len == 13);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("Multipart file test passed!\n");
}

static void test_multipart_missing_content_type() {
  printf("Testing multipart with missing Content-Type...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  ctx.request.body = strdup("test");
  ctx.request.body_len = 4;

  reset_counters();
  csilk_multipart_parse(&ctx, collect_part);
  assert(part_count == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("Multipart missing Content-Type test passed!\n");
}

static void test_multipart_null_body() {
  printf("Testing multipart with NULL body...\n");
  csilk_multipart_parse(NULL, collect_part);

  csilk_ctx_t ctx = {0};
  csilk_multipart_parse(&ctx, collect_part);

  printf("Multipart null body test passed!\n");
}

int main() {
  test_multipart_simple();
  test_multipart_file();
  test_multipart_missing_content_type();
  test_multipart_null_body();
  printf("test_multipart: ALL PASSED\n");
  return 0;
}
