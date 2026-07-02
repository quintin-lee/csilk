#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int
main()
{
    printf("Testing csilk_next with nullptr handlers...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    // This should not crash and should not increment handler_index
    csilk_next(ctx);

    assert(csilk_get_handler_index(ctx) == -1);

    printf("Testing csilk_next with empty handlers (just nullptr terminator)...\n");
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_next(ctx);
    // Should increment index to 0, see it's nullptr, and return
    assert(csilk_get_handler_index(ctx) == 0);

    csilk_test_ctx_free(ctx);

    printf("test_next_null: PASS\n");
    return 0;
}
