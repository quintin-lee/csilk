#include <assert.h>
#include <stdio.h>

#include "gin.h"

void test_server_init() {
  printf("Testing server initialization...\n");
  gin_router_t* router = gin_router_new();
  gin_server_t* server = gin_server_new(router);
  assert(server != NULL);

  gin_server_free(server);
  gin_router_free(router);
  printf("Server initialization test passed.\n");
}

int main() {
  test_server_init();
  printf("All server tests passed!\n");
  return 0;
}
