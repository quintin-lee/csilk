#include <setjmp.h>
#include <stdio.h>

#include "gin.h"

// Define a test that triggers a panic
void panic_handler(gin_ctx_t* c) { gin_panic(c); }

void normal_handler(gin_ctx_t* c) { gin_string(c, 200, "OK"); }

int main() {
  gin_ctx_t c;
  c.has_jump_buffer = 0;
  c.aborted = 0;
  c.handler_index = -1;
  gin_handler_t handlers[] = {gin_recovery_handler, panic_handler,
                              normal_handler, NULL};
  c.handlers = handlers;

  printf("Testing recovery...\n");
  gin_next(&c);

  if (c.response.status == 500) {
    printf("Recovered from panic! Status: %d\n", c.response.status);
    gin_ctx_cleanup(&c);
    return 0;  // Test passed
  } else {
    printf("Failed to recover! Status: %d\n", c.response.status);
    gin_ctx_cleanup(&c);
    return 1;  // Test failed
  }
}
