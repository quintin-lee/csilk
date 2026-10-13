#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/util/csv.h"

void
test_csv_escape(void)
{
    char buf[128];

    /* Plain string - no quotes */
    csilk_csv_escape(buf, sizeof(buf), "hello");
    assert(strcmp(buf, "hello") == 0);

    /* String with comma - must be quoted */
    csilk_csv_escape(buf, sizeof(buf), "hello,world");
    assert(strcmp(buf, "\"hello,world\"") == 0);

    /* String with double quote - must be escaped as "" */
    csilk_csv_escape(buf, sizeof(buf), "foo\"bar");
    assert(strcmp(buf, "\"foo\"\"bar\"") == 0);

    /* String with newline */
    csilk_csv_escape(buf, sizeof(buf), "line1\nline2");
    assert(strcmp(buf, "\"line1\nline2\"") == 0);

    /* Null string produces empty */
    csilk_csv_escape(buf, sizeof(buf), NULL);
    assert(strcmp(buf, "") == 0);

    printf("test_csv_escape passed\n");
}

void
test_csv_parse_field(void)
{
    char   field[128];
    size_t consumed = 0;

    /* Simple unquoted */
    assert(csilk_csv_parse_field("foo,bar", 7, field, sizeof(field), &consumed) == 0);
    assert(strcmp(field, "foo") == 0);
    assert(consumed == 3);

    /* Quoted with comma */
    assert(csilk_csv_parse_field("\"foo,bar\",baz", 13, field, sizeof(field), &consumed) == 0);
    assert(strcmp(field, "foo,bar") == 0);
    assert(consumed == 9);

    /* Quoted with doubled quote */
    assert(csilk_csv_parse_field("\"foo\"\"bar\",baz", 14, field, sizeof(field), &consumed) == 0);
    assert(strcmp(field, "foo\"bar") == 0);
    assert(consumed == 10);

    printf("test_csv_parse_field passed\n");
}

void
test_csv_parse_row(void)
{
    char line[] = "2026-10-13,\"Coffee, Milk\",4.50,\"Note with \"\"special\"\" quote\"\r\n";
    char out[12][512];
    int  count = 0;

    assert(csilk_csv_parse_row(line, strlen(line), out, &count) == 4);
    assert(count == 4);
    assert(strcmp(out[0], "2026-10-13") == 0);
    assert(strcmp(out[1], "Coffee, Milk") == 0);
    assert(strcmp(out[2], "4.50") == 0);
    assert(strcmp(out[3], "Note with \"special\" quote") == 0);

    printf("test_csv_parse_row passed\n");
}

int
main(void)
{
    test_csv_escape();
    test_csv_parse_field();
    test_csv_parse_row();
    printf("test_csv: ALL PASSED\n");
    return 0;
}
