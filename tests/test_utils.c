#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

void test_sha1_rfc3174_1() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];

  csilk_sha1_init(&ctx);
  csilk_sha1_update(&ctx, (uint8_t*)"abc", 3);
  csilk_sha1_final(&ctx, digest);

  uint8_t expected[] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81,
                        0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50,
                        0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
  assert(memcmp(digest, expected, 20) == 0);
  printf("test_sha1_rfc3174_1 passed\n");
}

void test_sha1_rfc3174_2() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];

  csilk_sha1_init(&ctx);
  csilk_sha1_update(&ctx, (uint8_t*)"abcdbcdecdefdefgefghfghighij"
                                     "hijkijkljklmklmnlmnomnopnopq",
                     56);
  csilk_sha1_final(&ctx, digest);

  uint8_t expected[] = {0x84, 0x98, 0x3E, 0x44, 0x1C, 0x3B, 0xD2,
                        0x6E, 0xBA, 0xAE, 0x4A, 0xA1, 0xF9, 0x51,
                        0x29, 0xE5, 0xE5, 0x46, 0x70, 0xF1};
  assert(memcmp(digest, expected, 20) == 0);
  printf("test_sha1_rfc3174_2 passed\n");
}

void test_sha1_empty() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];

  csilk_sha1_init(&ctx);
  csilk_sha1_final(&ctx, digest);

  uint8_t expected[] = {0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B,
                        0x0D, 0x32, 0x55, 0xBF, 0xEF, 0x95, 0x60,
                        0x18, 0x90, 0xAF, 0xD8, 0x07, 0x09};
  assert(memcmp(digest, expected, 20) == 0);
  printf("test_sha1_empty passed\n");
}

void test_sha1_single_char() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];

  csilk_sha1_init(&ctx);
  csilk_sha1_update(&ctx, (uint8_t*)"a", 1);
  csilk_sha1_final(&ctx, digest);

  uint8_t expected[] = {0x86, 0xF7, 0xE4, 0x37, 0xFA, 0xA5, 0xA7,
                        0xFC, 0xE1, 0x5D, 0x1D, 0xDC, 0xB9, 0xEA,
                        0xEA, 0xEA, 0x37, 0x76, 0x67, 0xB8};
  assert(memcmp(digest, expected, 20) == 0);
  printf("test_sha1_single_char passed\n");
}

void test_sha1_two_updates() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];

  csilk_sha1_init(&ctx);
  csilk_sha1_update(&ctx, (uint8_t*)"ab", 2);
  csilk_sha1_update(&ctx, (uint8_t*)"c", 1);
  csilk_sha1_final(&ctx, digest);

  uint8_t expected[] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81,
                        0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50,
                        0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
  assert(memcmp(digest, expected, 20) == 0);
  printf("test_sha1_two_updates passed (same as abc)\n");
}

void test_base64_rfc4648_1() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"", 0, out);
  assert(strcmp(out, "") == 0);
  printf("test_base64_rfc4648_1 passed\n");
}

void test_base64_rfc4648_2() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"f", 1, out);
  assert(strcmp(out, "Zg==") == 0);
  printf("test_base64_rfc4648_2 passed\n");
}

void test_base64_rfc4648_3() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"fo", 2, out);
  assert(strcmp(out, "Zm8=") == 0);
  printf("test_base64_rfc4648_3 passed\n");
}

void test_base64_rfc4648_4() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"foo", 3, out);
  assert(strcmp(out, "Zm9v") == 0);
  printf("test_base64_rfc4648_4 passed\n");
}

void test_base64_rfc4648_5() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"foob", 4, out);
  assert(strcmp(out, "Zm9vYg==") == 0);
  printf("test_base64_rfc4648_5 passed\n");
}

void test_base64_rfc4648_6() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"fooba", 5, out);
  assert(strcmp(out, "Zm9vYmE=") == 0);
  printf("test_base64_rfc4648_6 passed\n");
}

void test_base64_rfc4648_7() {
  char out[64];
  memset(out, 0, sizeof(out));

  csilk_base64_encode((uint8_t*)"foobar", 6, out);
  assert(strcmp(out, "Zm9vYmFy") == 0);
  printf("test_base64_rfc4648_7 passed\n");
}

void test_base64_binary() {
  char out[64];
  memset(out, 0, sizeof(out));

  uint8_t raw[] = {0x14, 0xFB, 0x9C, 0x03, 0xD9, 0x7E};
  csilk_base64_encode(raw, 6, out);
  assert(strcmp(out, "FPucA9l+") == 0);
  printf("test_base64_binary passed\n");
}

void test_sha1_base64_ws_accept() {
  csilk_sha1_ctx ctx;
  uint8_t digest[20];
  char accept_key[64];

  const char* key = "dGhlIHNhbXBsZSBub25jZQ==";
  const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char combined[256];
  snprintf(combined, sizeof(combined), "%s%s", key, guid);

  csilk_sha1_init(&ctx);
  csilk_sha1_update(&ctx, (uint8_t*)combined, strlen(combined));
  csilk_sha1_final(&ctx, digest);

  memset(accept_key, 0, sizeof(accept_key));
  csilk_base64_encode(digest, 20, accept_key);

  assert(strcmp(accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
  printf("test_sha1_base64_ws_accept passed\n");
}

int main() {
  test_sha1_rfc3174_1();
  test_sha1_rfc3174_2();
  test_sha1_empty();
  test_sha1_single_char();
  test_sha1_two_updates();
  test_base64_rfc4648_1();
  test_base64_rfc4648_2();
  test_base64_rfc4648_3();
  test_base64_rfc4648_4();
  test_base64_rfc4648_5();
  test_base64_rfc4648_6();
  test_base64_rfc4648_7();
  test_base64_binary();
  test_sha1_base64_ws_accept();
  printf("test_utils: ALL PASSED\n");
  return 0;
}
