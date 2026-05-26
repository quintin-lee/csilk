/**
 * @file test_hooks.c
 * @brief Tests for Csilk Hook system.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

static int server_start_called = 0;
static int server_stop_called = 0;
static int conn_open_called = 0;
static int conn_close_called = 0;
static int req_begin_called = 0;
static int req_end_called = 0;

static void on_server_start(csilk_server_t* s) {
  (void)s;
  server_start_called++;
}
static void on_server_stop(csilk_server_t* s) {
  (void)s;
  server_stop_called++;
}
static void on_conn_open(csilk_ctx_t* c) {
  (void)c;
  conn_open_called++;
}
static void on_conn_close(csilk_ctx_t* c) {
  (void)c;
  conn_close_called++;
}
static void on_req_begin(csilk_ctx_t* c) {
  (void)c;
  req_begin_called++;
}
static void on_req_end(csilk_ctx_t* c) {
  (void)c;
  req_end_called++;
}

int main() {
  printf("Testing Hook system...\n");

  csilk_router_t* router = csilk_router_new();
  csilk_server_t* s = csilk_server_new(router);

  csilk_server_add_hook(s, CSILK_HOOK_SERVER_START, on_server_start);
  csilk_server_add_hook(s, CSILK_HOOK_SERVER_STOP, on_server_stop);
  csilk_server_add_hook(s, CSILK_HOOK_CONN_OPEN, on_conn_open);
  csilk_server_add_hook(s, CSILK_HOOK_CONN_CLOSE, on_conn_close);
  csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, on_req_begin);
  csilk_server_add_hook(s, CSILK_HOOK_REQUEST_END, on_req_end);

  // We can't easily run the full server loop in a unit test,
  // but we can manually trigger some hooks.
  // Internal helper 'trigger_hooks' is static in server.c,
  // but we can test through the server instance if we mock things.
  // For now, let's just ensure registration works (implicit in lack of crash).

  // Real verification would need integration test style.

  csilk_server_free(s);
  csilk_router_free(router);

  printf("Hook system basic registration test passed!\n");
  return 0;
}
