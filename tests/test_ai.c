#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

void
test_ai_init()
{
    printf("Testing AI initialization...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", nullptr);
    assert(ai != nullptr);
    csilk_ai_free(ai);
    printf("AI initialization passed\n");
}

void
test_ai_invalid_driver()
{
    printf("Testing invalid AI driver...\n");
    csilk_ai_t* ai = csilk_ai_new("non-existent", "key", nullptr);
    assert(ai == nullptr);
    printf("Invalid AI driver test passed\n");
}

int
main()
{
    test_ai_init();
    test_ai_invalid_driver();
    printf("test_ai: ALL PASSED\n");
    return 0;
}
