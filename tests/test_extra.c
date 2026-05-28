/**
 * @file test_extra.c
 * @brief Tests for Request ID and Health Check.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

void dummy_handler(csilk_ctx_t* c) { (void)c; }

void test_request_id() {
  printf("Testing Request ID middleware...\n");
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);
  csilk_handler_t handlers[] = {csilk_request_id_middleware, dummy_handler,
                                NULL};
  c.handlers = handlers;
  c.handler_index = -1;

  /* Initially empty */
  assert(c.request_id[0] == '\0');

  /* Apply middleware (manually call first handler) */
  c.handler_index++;
  c.handlers[c.handler_index](&c);

  /* Should be generated */
  assert(c.request_id[0] != '\0');
  assert(strlen(c.request_id) == 36); /* UUID length */

  /* Should be in headers */
  const char* header_id = csilk_get_response_header(&c, "X-Request-Id");
  assert(header_id != NULL);
  assert(strcmp(header_id, c.request_id) == 0);

  /* Second call should not change it */
  char first_id[37];
  strcpy(first_id, c.request_id);
  c.handler_index = -1;
  c.handler_index++;
  c.handlers[c.handler_index](&c);
  assert(strcmp(first_id, c.request_id) == 0);

  csilk_ctx_cleanup(&c);
  assert(c.request_id[0] == '\0');
  csilk_arena_free(c.arena);
}

void test_health_check() {
  printf("Testing Health Check handler...\n");
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  csilk_health_check_handler(&c);

  assert(c.response.status == CSILK_STATUS_OK);
  assert(c.response.body != NULL);
  assert(strstr(c.response.body, "\"status\":\"up\"") != NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
}

int main() {
  test_request_id();
  test_health_check();
  printf("All extra tests passed!\n");
  return 0;
}
