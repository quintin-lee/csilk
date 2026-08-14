#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/io/af_xdp_internal.h"

int csilk_str_view_equal(csilk_str_view_t view, const char* str);
int csilk_http1_parse_header_slices(const char*           buf,
                                    size_t                len,
                                    csilk_header_slice_t* slices,
                                    size_t                max_slices,
                                    size_t*               out_slice_count);

static void
test_header_slice_parsing(void)
{
    const char* req = "GET /api/v1/resource HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "User-Agent: csilk-test-agent\r\n"
                      "Accept: application/json\r\n"
                      "\r\n";

    csilk_header_slice_t slices[8];
    size_t               count = 0;

    int res = csilk_http1_parse_header_slices(req, strlen(req), slices, 8, &count);
    assert(res == 0);
    assert(count == 3);

    assert(csilk_str_view_equal(slices[0].name, "Host"));
    assert(csilk_str_view_equal(slices[0].value, "localhost:8080"));

    assert(csilk_str_view_equal(slices[1].name, "User-Agent"));
    assert(csilk_str_view_equal(slices[1].value, "csilk-test-agent"));

    assert(csilk_str_view_equal(slices[2].name, "Accept"));
    assert(csilk_str_view_equal(slices[2].value, "application/json"));

    printf("test_header_slice_parsing passed\n");
}

static void
test_view_utilities(void)

{
    /* Constructors */
    csilk_view_t v1 = csilk_view("hello world", 5);
    assert(v1.len == 5);
    assert(strncmp(v1.ptr, "hello", 5) == 0);
    assert(strncmp(v1.data, "hello", 5) == 0);
    assert(!csilk_view_is_empty(v1));

    csilk_view_t v_empty = csilk_view(NULL, 0);
    assert(csilk_view_is_empty(v_empty));

    csilk_view_t v_str = csilk_view_from_str("csilk framework");
    assert(v_str.len == 15);
    assert(!csilk_view_is_empty(v_str));

    /* Comparison */
    assert(csilk_view_cmp(v1, "hello") == 0);
    assert(csilk_view_cmp(v1, "hell") > 0);
    assert(csilk_view_cmp(v1, "hello world") < 0);

    /* Case-insensitive comparison */
    assert(csilk_view_casecmp(v1, "HELLO") == 0);
    assert(csilk_view_casecmp(v1, "HeLLo") == 0);
    assert(csilk_view_casecmp(v1, "WORLD") < 0);

    /* Equality */
    csilk_view_t v2 = csilk_view("hello there", 5);
    assert(csilk_view_equal(v1, v2) == 1);
    csilk_view_t v3 = csilk_view("world", 5);
    assert(csilk_view_equal(v1, v3) == 0);

    /* Heap materialization */
    char* heap_s = csilk_view_to_heap(v1);
    assert(heap_s != NULL);
    assert(strcmp(heap_s, "hello") == 0);
    free(heap_s);

    printf("test_view_utilities passed\n");
}

int
main(void)
{
    test_header_slice_parsing();
    test_view_utilities();
    printf("All test_http1_zerocopy tests passed successfully!\n");
    return 0;
}
