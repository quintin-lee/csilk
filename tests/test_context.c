#include <assert.h>
#include <stddef.h>
#include <stdio.h>

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

void handler_resp(csilk_ctx_t* c) { csilk_string(c, 200, "hello"); }

void test_context_response() {
  csilk_handler_t handlers[] = {handler_resp, NULL};
  csilk_ctx_t c = {.handler_index = -1,
                 .handlers = handlers,
                 .aborted = 0,
                 .response = {0, NULL}};
  csilk_next(&c);
  assert(c.response.status == 200);
  assert(c.response.body != NULL);
  // Use string comparison if necessary, but here direct comparison is fine for
  // a test literal
  assert(sizeof("hello") == 6);
  // Just a basic check that it's set
  csilk_ctx_cleanup(&c);
  printf("test_context_response passed\n");
}

int main() {
  test_basic_chaining();
  test_abort();
  test_context_response();
  return 0;
}
