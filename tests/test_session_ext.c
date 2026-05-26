#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static uint32_t hash_key(const char* key) {
  uint32_t hash = 5381;
  int c;
  while ((c = (unsigned char)*key++)) hash = ((hash << 5) + hash) + tolower(c);
  return hash % 64;
}

static void set_request_header(csilk_ctx_t* c, const char* key,
                               const char* value) {
  csilk_header_t* h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
  assert(h != NULL);
  h->key = csilk_arena_strdup(c->arena, key);
  h->value = csilk_arena_strdup(c->arena, value);
  h->key_len = strlen(h->key);
  h->value_len = strlen(h->value);
  uint32_t bucket = hash_key(key);
  h->next = c->request.headers.buckets[bucket];
  c->request.headers.buckets[bucket] = h;
}

static void test_session_destroy(void) {
  csilk_session_init();
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(256);
  csilk_session_start(&c);
  void* session = csilk_get(&c, "_session");
  assert(session != NULL);
  csilk_session_destroy(&c);
  void* gone = csilk_get(&c, "_session");
  assert(gone == NULL);
  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("  session_destroy passed\n");
}

static void test_session_resume(void) {
  csilk_session_init();

  csilk_ctx_t ctx1 = {0};
  ctx1.arena = csilk_arena_new(512);
  csilk_session_start(&ctx1);
  void* s1 = csilk_get(&ctx1, "_session");
  assert(s1 != NULL);

  const char* set_cookie = csilk_get_response_header(&ctx1, "Set-Cookie");
  assert(set_cookie != NULL);
  assert(strstr(set_cookie, "csilk_session=") != NULL);

  const char* eq = strchr(set_cookie, '=');
  assert(eq != NULL);
  eq++;
  const char* semicolon = strchr(eq, ';');
  size_t id_len = semicolon ? (size_t)(semicolon - eq) : strlen(eq);

  char cookie_buf[256];
  snprintf(cookie_buf, sizeof(cookie_buf), "csilk_session=%.*s", (int)id_len,
           eq);

  csilk_ctx_t ctx2 = {0};
  ctx2.request.path = strdup("/resume");
  ctx2.arena = csilk_arena_new(512);
  set_request_header(&ctx2, "Cookie", cookie_buf);

  csilk_session_start(&ctx2);
  void* s2 = csilk_get(&ctx2, "_session");
  assert(s2 != NULL);
  assert(s2 == s1);

  csilk_session_destroy(&ctx2);
  csilk_ctx_cleanup(&ctx1);
  csilk_ctx_cleanup(&ctx2);
  csilk_arena_free(ctx1.arena);
  csilk_arena_free(ctx2.arena);
  free(ctx2.request.path);
  printf("  session_resume passed\n");
}

static void test_session_get_no_session(void) {
  csilk_session_init();
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(256);
  void* val = csilk_session_get(&c, "any");
  assert(val == NULL);
  csilk_session_set(&c, "k", NULL);
  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("  session_get_no_session passed\n");
}

static void test_session_set_get_null(void) {
  csilk_session_init();
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(256);
  csilk_session_set(&c, NULL, NULL);
  csilk_session_set(NULL, "k", NULL);
  assert(csilk_session_get(&c, NULL) == NULL);
  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("  session_set_get_null passed\n");
}

int main() {
  printf("Testing session destroy...\n");
  test_session_destroy();

  printf("Testing session resume (find existing)...\n");
  test_session_resume();

  printf("Testing session get with no session...\n");
  test_session_get_no_session();

  printf("Testing session set/get NULL safety...\n");
  test_session_set_get_null();

  printf("test_session_ext: PASS\n");
  return 0;
}
