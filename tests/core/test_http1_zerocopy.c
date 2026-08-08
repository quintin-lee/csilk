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

int
main(void)
{
    test_header_slice_parsing();
    printf("All test_http1_zerocopy tests passed successfully!\n");
    return 0;
}
