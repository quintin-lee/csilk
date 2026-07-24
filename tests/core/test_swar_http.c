#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/core/swar_http.h"

int
main(void)
{
    printf("Testing SWAR / SIMD Branchless HTTP Parser...\n");

    const char* text =
        "GET /api/v1/resource HTTP/1.1\r\nHost: example.com\r\nAccept: application/json\r\n\r\n";
    size_t len = strlen(text);

    /* Test 1: find_char */
    size_t colon = csilk_swar_find_char(text, len, ':');
    assert(colon != (size_t)-1);

    /* Test 2: find_crlf */
    size_t crlf = csilk_swar_find_crlf(text, len);
    assert(crlf == 29);

    /* Test 3: parse_header_line */
    const char*      line = "Content-Type: application/json; charset=utf-8\r\n";
    csilk_str_view_t field, value;
    assert(csilk_swar_parse_header_line(line, strlen(line), &field, &value) == 0);
    assert(field.len == 12);
    assert(strncmp(field.data, "Content-Type", 12) == 0);
    assert(value.len == 31);
    assert(strncmp(value.data, "application/json; charset=utf-8", 31) == 0);

    printf("test_swar_http: PASS\n");
    return 0;
}
