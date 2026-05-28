#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static const char* get_resp_header(csilk_ctx_t* c, const char* key) {
  uint32_t hash = 5381;
  int ch;
  const char* k = key;
  while ((ch = (unsigned char)*k++)) {
    hash = ((hash << 5) + hash) + tolower(ch);
  }
  uint32_t bucket = hash % CSILK_HEADER_BUCKETS;

  csilk_header_t* h = c->response.headers.buckets[bucket];
  while (h) {
    if (strcasecmp(h->key, key) == 0) return h->value;
    h = h->next;
  }
  return NULL;
}

static void test_redirect_basic() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  memset(&c.response.headers, 0, sizeof(csilk_header_map_t));

  csilk_redirect(&c, CSILK_STATUS_FOUND, "/new-location");

  assert(c.response.status == CSILK_STATUS_FOUND);
  assert(c.aborted == 1);

  const char* loc = get_resp_header(&c, "Location");
  assert(loc != NULL);
  assert(strcmp(loc, "/new-location") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_redirect_basic passed\n");
}

static void test_redirect_simple() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  memset(&c.response.headers, 0, sizeof(csilk_header_map_t));

  csilk_redirect_simple(&c, "/redirect-simple");

  assert(c.response.status == CSILK_STATUS_FOUND);
  assert(c.aborted == 1);

  const char* loc = get_resp_header(&c, "Location");
  assert(loc != NULL);
  assert(strcmp(loc, "/redirect-simple") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_redirect_simple passed\n");
}

static void test_redirect_status_codes() {
  int codes[] = {
      CSILK_STATUS_MOVED_PERMANENTLY,
      CSILK_STATUS_FOUND,
      CSILK_STATUS_TEMPORARY_REDIRECT,
  };

  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);
    memset(&c.response.headers, 0, sizeof(csilk_header_map_t));

    csilk_redirect(&c, codes[i], "/target");

    assert(c.response.status == codes[i]);
    assert(c.aborted == 1);

    const char* loc = get_resp_header(&c, "Location");
    assert(loc != NULL);
    assert(strcmp(loc, "/target") == 0);

    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
  }
  printf("test_redirect_status_codes passed\n");
}

static void test_redirect_null_safety() {
  csilk_redirect(NULL, CSILK_STATUS_FOUND, "/nowhere");
  csilk_redirect_simple(NULL, "/nowhere");

  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  memset(&c.response.headers, 0, sizeof(csilk_header_map_t));

  csilk_redirect(&c, CSILK_STATUS_FOUND, NULL);
  assert(c.aborted == 0);
  assert(c.response.status == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_redirect_null_safety passed\n");
}

int main() {
  test_redirect_basic();
  test_redirect_simple();
  test_redirect_status_codes();
  test_redirect_null_safety();
  printf("test_redirect: ALL PASSED\n");
  return 0;
}
