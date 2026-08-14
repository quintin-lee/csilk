#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int g_h1_called = 0;
static int g_h2_called = 0;

static void
mock_h1(csilk_ctx_t* c)

{
    g_h1_called++;
    csilk_next(c);
}

static void
mock_h2(csilk_ctx_t* c)
{
    g_h2_called++;
    csilk_next(c);
}

int
main()
{
    printf("Testing csilk_next with nullptr context...\n");
    csilk_next(NULL);

    printf("Testing csilk_next with nullptr handlers...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    // This should not crash and should not increment handler_index
    csilk_next(ctx);
    assert(csilk_get_handler_index(ctx) == -1);
    assert(csilk_get_handler_count(ctx) == 0);

    printf("Testing csilk_next with empty handlers (just nullptr terminator)...\n");
    csilk_handler_t handlers_empty[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers_empty);

    csilk_next(ctx);
    // Should increment index to 0, see it's nullptr, and return
    assert(csilk_get_handler_index(ctx) == 0);

    printf("Testing csilk_next chain execution and bounds check...\n");
    csilk_ctx_t*    ctx2 = csilk_test_ctx_new();
    csilk_handler_t chain[] = {mock_h1, mock_h2, nullptr};
    csilk_test_ctx_set_handlers(ctx2, chain);
    assert(csilk_get_handler_count(ctx2) == 2);

    csilk_next(ctx2);
    assert(g_h1_called == 1);
    assert(g_h2_called == 1);
    // handler_index reaches 2 (bounds limit), terminating chain safely
    assert(csilk_get_handler_index(ctx2) == 2);

    // Subsequent calls to csilk_next past end of chain should be safely bounded
    csilk_next(ctx2);
    assert(csilk_get_handler_index(ctx2) == 3);
    assert(g_h1_called == 1);
    assert(g_h2_called == 1);

    csilk_test_ctx_free(ctx2);
    csilk_test_ctx_free(ctx);

    printf("test_next_null: PASS\n");
    return 0;
}
