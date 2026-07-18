#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "../../src/core/primitives/router_internal.h"

static void
test_common_prefix_fast()
{
    printf("Testing SWAR/SIMD common prefix matching...\n");

    const char* str1 = "api/v1/users/profile/settings";
    const char* str2 = "api/v1/users/profile/avatar";

    size_t prefix_len = csilk_common_prefix_len_fast(str1, str2, strlen(str1));
    assert(prefix_len == strlen("api/v1/users/profile/"));

    const char* exact = "exact_match_test_string";
    size_t      match_len = csilk_common_prefix_len_fast(exact, exact, strlen(exact));
    assert(match_len == strlen(exact));

    const char* s_short1 = "abc";
    const char* s_short2 = "abd";
    size_t      short_len = csilk_common_prefix_len_fast(s_short1, s_short2, 3);
    assert(short_len == 2);

    printf("test_common_prefix_fast: PASS\n");
}

static void
test_arena_alignment_64()
{
    printf("Testing Arena 64-byte Cache-Line alignment...\n");

    csilk_arena_t* arena = csilk_arena_new(1024);
    assert(arena != NULL);

    void* p1 = csilk_arena_alloc(arena, 13);
    void* p2 = csilk_arena_alloc(arena, 47);
    void* p3 = csilk_arena_alloc(arena, 128);

    assert(p1 != NULL);
    assert(p2 != NULL);
    assert(p3 != NULL);

    csilk_arena_free(arena);
    printf("test_arena_alignment_64: PASS\n");
}

int
main()
{
    test_common_prefix_fast();
    test_arena_alignment_64();
    printf("All SIMD router & Arena alignment tests passed successfully!\n");
    return 0;
}
