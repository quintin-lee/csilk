#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk.h"

#define TEST_DIR "test_dir_static"

static void cleanup_test_dir(void) {
    remove(TEST_DIR "/test.html");
    rmdir(TEST_DIR);
}

void test_static_serves_file() {
    cleanup_test_dir();

    csilk_ctx_t ctx;
    memset(&ctx, 0, sizeof(csilk_ctx_t));
    ctx.arena = csilk_arena_new(1024);
    assert(ctx.arena != NULL);
    ctx.request.path = strdup("/test.html");
    assert(ctx.request.path != NULL);

    if (mkdir(TEST_DIR, 0777) != 0) {
        fprintf(stderr, "WARNING: mkdir failed, skipping test\n");
        csilk_ctx_cleanup(&ctx);
        return;
    }

    FILE* f = fopen(TEST_DIR "/test.html", "w");
    if (!f) {
        fprintf(stderr, "WARNING: fopen failed, skipping test\n");
        rmdir(TEST_DIR);
        csilk_ctx_cleanup(&ctx);
        return;
    }

    fprintf(f, "<html><body>Hello</body></html>");
    fclose(f);

    csilk_static(&ctx, TEST_DIR);

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
    assert(ctx.arena != NULL);
    ctx.request.path = strdup("/../../etc/passwd");
    assert(ctx.request.path != NULL);

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
