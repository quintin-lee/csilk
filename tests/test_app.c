#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_app.h"

static void test_app_new_free() {
  printf("Testing csilk_app_new and csilk_app_free...\n");
  csilk_app_t* app = csilk_app_new(NULL);
  assert(app != NULL);

  csilk_router_t* router = csilk_app_router(app);
  assert(router != NULL);

  csilk_server_t* server = csilk_app_server(app);
  assert(server != NULL);

  csilk_app_free(app);
  printf("App new/free test passed!\n");
}

static void test_app_route() {
  printf("Testing app route registration...\n");
  csilk_app_t* app = csilk_app_new(NULL);
  assert(app != NULL);

  csilk_app_get(app, "/test", NULL);
  csilk_app_post(app, "/test", NULL);
  csilk_app_put(app, "/test", NULL);
  csilk_app_delete(app, "/test", NULL);

  csilk_app_free(app);
  printf("App route test passed!\n");
}

static void test_app_server_config() {
  printf("Testing app server config...\n");
  csilk_app_t* app = csilk_app_new(NULL);
  assert(app != NULL);

  csilk_server_config_t cfg = {.idle_timeout_ms = 10000,
                               .max_body_size = 4096,
                               .max_header_size = 1024,
                               .listen_backlog = 64,
                               .tcp_nodelay = 1,
                               .tcp_keepalive = 0,
                               .worker_threads = 1};
  csilk_app_set_server_config(app, cfg);

  csilk_config_t* cfg_copy = csilk_app_config(app);
  assert(cfg_copy != NULL);
  assert(cfg_copy->server.idle_timeout_ms == 10000);
  assert(cfg_copy->server.max_body_size == 4096);
  csilk_config_free(cfg_copy);
  free(cfg_copy);

  csilk_app_free(app);
  printf("App server config test passed!\n");
}

int main() {
  test_app_new_free();
  test_app_route();
  test_app_server_config();
  printf("test_app: ALL PASSED\n");
  return 0;
}
