#include <setjmp.h>
#include <stdio.h>

#include "csilk.h"

// Define a test that triggers a panic
void panic_handler(csilk_ctx_t* c) { csilk_panic(c); }

void normal_handler(csilk_ctx_t* c) { csilk_string(c, 200, "OK"); }

int main() {
  csilk_ctx_t c;
  c.has_jump_buffer = 0;
  c.aborted = 0;
  c.handler_index = -1;
  csilk_handler_t handlers[] = {csilk_recovery_handler, panic_handler,
                              normal_handler, NULL};
  c.handlers = handlers;

  printf("Testing recovery...\n");
  csilk_next(&c);

  if (c.response.status == 500) {
    printf("Recovered from panic! Status: %d\n", c.response.status);
    csilk_ctx_cleanup(&c);
    return 0;  // Test passed
  } else {
    printf("Failed to recover! Status: %d\n", c.response.status);
    csilk_ctx_cleanup(&c);
    return 1;  // Test failed
  }
}
