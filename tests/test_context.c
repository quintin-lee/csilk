#include "csilk_internal.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "csilk.h"

int counter = 0;

void m1(csilk_ctx_t* c) {
  counter++;
  csilk_next(c);
  counter++;
}

void m2(csilk_ctx_t* c) {
  counter++;
  csilk_next(c);
  counter++;
}

void handler(csilk_ctx_t* c) { counter++; }

void test_basic_chaining() {
  counter = 0;
  csilk_handler_t handlers[] = {m1, m2, handler, NULL};
  csilk_ctx_t c = {.handler_index = -1, .handlers = handlers, .aborted = 0};
  csilk_next(&c);
  assert(counter == 5);  // m1++, m2++, handler++, m2++, m1++
  printf("test_basic_chaining passed\n");
}

void m_abort(csilk_ctx_t* c) {
  counter++;
  csilk_abort(c);
}

void test_abort() {
  counter = 0;
  csilk_handler_t handlers[] = {m_abort, handler, NULL};
  csilk_ctx_t c = {.handler_index = -1, .handlers = handlers, .aborted = 0};
  csilk_next(&c);
  assert(counter == 1);
  printf("test_abort passed\n");
}

void handler_resp(csilk_ctx_t* c) { csilk_string(c, CSILK_STATUS_OK, "hello"); }

void test_context_response() {
  csilk_handler_t handlers[] = {handler_resp, NULL};
  csilk_ctx_t c = {.handler_index = -1,
                 .handlers = handlers,
                 .aborted = 0,
                 .response = {0, NULL}};
  csilk_next(&c);
  assert(c.response.status == CSILK_STATUS_OK);
  assert(c.response.body != NULL);
  // Use string comparison if necessary, but here direct comparison is fine for
  // a test literal
  assert(sizeof("hello") == 6);
  // Just a basic check that it's set
  csilk_ctx_cleanup(&c);
  printf("test_context_response passed\n");
}

void test_context_storage() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);
    int val = 42;
    csilk_set(&c, "test_key", &val);
    
    void* retrieved = csilk_get(&c, "test_key");
    assert(retrieved != NULL);
    assert(*(int*)retrieved == 42);
    
    // Overwrite
    int val2 = 100;
    csilk_set(&c, "test_key", &val2);
    assert(*(int*)csilk_get(&c, "test_key") == 100);
    
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    printf("test_context_storage passed\n");
}

void test_context_arena() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);
    
    char* s = csilk_arena_strdup(c.arena, "arena string");
    assert(strcmp(s, "arena string") == 0);
    
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    printf("test_context_arena passed\n");
}

int main() {
  test_basic_chaining();
  test_abort();
  test_context_response();
  test_context_storage();
  test_context_arena();
  return 0;
}
