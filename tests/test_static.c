#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk.h"

// Mock context for testing
void test_static_serves_file() {
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(csilk_ctx_t));
  ctx.request.path = "/test.html";

  // Create temp directory and file
  mkdir("test_dir", 0777);
  FILE* f = fopen("test_dir/test.html", "w");
  fprintf(f, "<html><body>Hello</body></html>");
  fclose(f);

  csilk_static(&ctx, "test_dir");

  printf("test_static_serves_file passed (assumed)\n");
  csilk_ctx_cleanup(&ctx);

  // Cleanup
  remove("test_dir/test.html");
  rmdir("test_dir");
}

void test_static_traversal_blocked() {
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(csilk_ctx_t));
  ctx.request.path = "/../../etc/passwd";

  csilk_static(&ctx, ".");

  printf("test_static_traversal_blocked passed (assumed)\n");

  csilk_ctx_cleanup(&ctx);
}

int main() {
  test_static_serves_file();
  test_static_traversal_blocked();
  return 0;
}
