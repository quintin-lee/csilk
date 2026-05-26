#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

static int dummy_handler_called;

static void dummy_handler(csilk_ctx_t* c) { dummy_handler_called++; }

int main() {
  csilk_router_t* r = csilk_router_new();
  csilk_server_t* srv = csilk_server_new(r);
  assert(srv != NULL);

  printf("Testing csilk_server_set_not_found_handler...\n");
  {
    dummy_handler_called = 0;
    csilk_server_set_not_found_handler(srv, dummy_handler);
    csilk_server_set_not_found_handler(NULL, dummy_handler);
    csilk_server_set_not_found_handler(srv, NULL);
  }

  printf("Testing csilk_server_set_spa_fallback...\n");
  {
    csilk_server_set_spa_fallback(srv, "/tmp/static");
    csilk_server_set_spa_fallback(NULL, "/tmp/static");
    csilk_server_set_spa_fallback(srv, NULL);
    csilk_server_set_spa_fallback(srv, "/tmp/other");
  }

  printf("Testing csilk_server_set_max_connections...\n");
  {
    int prev = csilk_server_set_max_connections(srv, 100);
    assert(prev >= 0);
    prev = csilk_server_set_max_connections(srv, 0);
    assert(prev == 100);
    prev = csilk_server_set_max_connections(NULL, 50);
    assert(prev == -1);
  }

  printf("Testing csilk_server_set_storage_driver...\n");
  {
    csilk_server_set_storage_driver(srv, NULL);
    csilk_server_set_storage_driver(NULL, NULL);
  }

  printf("Testing csilk_server_set_crypto_driver...\n");
  {
    csilk_server_set_crypto_driver(srv, NULL);
    csilk_server_set_crypto_driver(NULL, NULL);
  }

  printf("Testing NULL safety for all setters...\n");
  {
    csilk_server_set_not_found_handler(NULL, dummy_handler);
    csilk_server_set_spa_fallback(NULL, "/tmp/x");
    csilk_server_set_storage_driver(NULL, NULL);
    csilk_server_set_crypto_driver(NULL, NULL);
  }

  csilk_server_free(srv);
  csilk_router_free(r);

  printf("test_server_ext: PASS\n");
  return 0;
}
