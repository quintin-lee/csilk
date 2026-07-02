#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

// Mock _csilk_send_response
void
_csilk_send_response(csilk_ctx_t* c)
{
    (void)c;
}

static void
setup_test_file()
{
    FILE* f = fopen("./test_direct_file.txt", "w");
    fputs("Direct file content", f);
    fclose(f);
}

void
test_csilk_file_serves_direct()
{
    printf("Testing csilk_file direct serve...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_test_ctx_set_request(ctx, "GET", "/irrelevant");
    csilk_file(ctx, "./test_direct_file.txt");

    assert(csilk_is_async(ctx) == 1);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(csilk_get_status(ctx) == CSILK_STATUS_OK);
    assert(csilk_get_file_fd(ctx) != -1);

    csilk_test_ctx_free(ctx);
    printf("test_csilk_file_serves_direct passed\n");
}

void
test_csilk_file_not_found()
{
    printf("Testing csilk_file not found...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_file(ctx, "./non_existent_file.txt");

    assert(csilk_is_async(ctx) == 1);
    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(csilk_get_status(ctx) == CSILK_STATUS_NOT_FOUND);

    csilk_test_ctx_free(ctx);
    printf("test_csilk_file_not_found passed\n");
}

int
main()
{
    setup_test_file();
    test_csilk_file_serves_direct();
    test_csilk_file_not_found();

    remove("./test_direct_file.txt");
    return 0;
}
