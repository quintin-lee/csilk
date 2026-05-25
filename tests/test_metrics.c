/**
 * @file test_metrics.c
 * @brief Test for Prometheus metrics middleware.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "context_internal.h"
#include "csilk.h"

static void dummy_handler(csilk_ctx_t* c) {
  (void)c;
}

void test_metrics() {
  printf("Testing Metrics middleware...\n");
  
  csilk_handler_t handlers[] = { (csilk_handler_t)csilk_metrics_middleware, dummy_handler, NULL };
  csilk_ctx_t c = { .handler_index = -1, .handlers = handlers, .aborted = 0 };
  c.arena = csilk_arena_new(4096);

  /* Simulate a request through middleware */
  csilk_next(&c);

  /* Call metrics handler to get output */
  csilk_metrics_handler(&c);

  /* Verify output */
  assert(c.response.status == CSILK_STATUS_OK);
  assert(c.response.body != NULL);
  assert(strstr(c.response.body, "http_requests_total 1") != NULL);
  assert(strstr(c.response.body, "http_request_duration_microseconds") != NULL);

  /* Second request */
  c.handler_index = -1; /* reset for next run */
  csilk_next(&c);
  csilk_metrics_handler(&c);
  assert(strstr(c.response.body, "http_requests_total 2") != NULL);

  csilk_arena_free(c.arena);
  printf("Metrics test passed!\n");
}

int main() {
  test_metrics();
  return 0;
}
