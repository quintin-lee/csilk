#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk.h"

static void cleanup_test_dir(void) {
    remove("test_dir/test.html");
    rmdir("test_dir");
}

void test_static_serves_file() {
    cleanup_test_dir();

    csilk_ctx_t ctx;
    memset(&ctx, 0, sizeof(csilk_ctx_t));
    ctx.arena = csilk_arena_new(1024);
    ctx.request.path = strdup("/test.html");

    mkdir("test_dir", 0777);
    FILE* f = fopen("test_dir/test.html", "w");
    fprintf(f, "<html><body>Hello</body></html>");
    fclose(f);

    csilk_static(&ctx, "test_dir");

    assert(ctx.response.status == 200);
    assert(ctx.response.body != NULL);
    assert(strcmp(ctx.response.body, "<html><body>Hello</body></html>") == 0);

    printf("test_static_serves_file: PASS\n");
    csilk_ctx_cleanup(&ctx);
    cleanup_test_dir();
}

void test_static_traversal_blocked() {
    csilk_ctx_t ctx;
    memset(&ctx, 0, sizeof(csilk_ctx_t));
    ctx.arena = csilk_arena_new(1024);
    ctx.request.path = strdup("/../../etc/passwd");

    csilk_static(&ctx, ".");

    assert(ctx.response.status == 403 ||
           ctx.response.status == 404);

    printf("test_static_traversal_blocked: PASS\n");
    csilk_ctx_cleanup(&ctx);
}

int main() {
    test_static_serves_file();
    test_static_traversal_blocked();
    return 0;
}
