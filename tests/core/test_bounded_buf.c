/**
 * @file test_bounded_buf.c
 * @brief Unit tests for the zero-allocation bounded string buffer and JSON
 * paradise builder (src/core/primitives/bounded_buf.c).
 *
 * @copyright MIT License
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "csilk/core/bounded_buf.h"

#define BUFN 512
static char                 g_buf[BUFN];
static csilk_bounded_buf_t  g_bb;
static csilk_bounded_json_t g_jb;

static void
t_init_str_len(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, BUFN);
    assert(csilk_bounded_buf_len(&g_bb) == 0);
    assert(csilk_bounded_buf_overflow(&g_bb) == 0);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "") == 0);

    csilk_bounded_buf_reset(&g_bb);
    assert(csilk_bounded_buf_len(&g_bb) == 0);
}

static void
t_putc_puts(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, BUFN);
    csilk_bounded_buf_puts(&g_bb, NULL); /* NULL treated as empty */
    assert(csilk_bounded_buf_len(&g_bb) == 0);

    csilk_bounded_buf_putc(&g_bb, 'h');
    csilk_bounded_buf_puts(&g_bb, "ell");
    csilk_bounded_buf_putc(&g_bb, 'o');
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "hello") == 0);
    assert(csilk_bounded_buf_len(&g_bb) == 5);
}

static void
t_puti(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, BUFN);
    csilk_bounded_buf_puti(&g_bb, 0);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "0") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_puti(&g_bb, 12345);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "12345") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_puti(&g_bb, -42);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "-42") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_puti(&g_bb, INT64_MIN);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "-9223372036854775808") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_puti(&g_bb, INT64_MAX);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "9223372036854775807") == 0);
}

static void
t_putu(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, BUFN);
    csilk_bounded_buf_putu(&g_bb, 0);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "0") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_putu(&g_bb, 987654321u);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "987654321") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_putu(&g_bb, UINT64_MAX);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "18446744073709551615") == 0);
}

static void
t_putf(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, BUFN);
    csilk_bounded_buf_putf(&g_bb, 3.14159, 2);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "3.14") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_putf(&g_bb, -0.5, 1);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "-0.5") == 0);

    csilk_bounded_buf_reset(&g_bb);
    csilk_bounded_buf_putf(&g_bb, 2.0, 0);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "2") == 0);
}

static void
t_overflow(void)
{
    csilk_bounded_buf_init(&g_bb, g_buf, 4); /* capacity 4 => fits 3 chars + NUL */
    csilk_bounded_buf_puts(&g_bb, "abcdef");
    assert(csilk_bounded_buf_overflow(&g_bb) == 1);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "abc") == 0);

    csilk_bounded_buf_reset(&g_bb);
    assert(csilk_bounded_buf_overflow(&g_bb) == 0);
    csilk_bounded_buf_putc(&g_bb, 'x');
    assert(csilk_bounded_buf_len(&g_bb) == 1);
    csilk_bounded_buf_putc(&g_bb, 'y');
    csilk_bounded_buf_putc(&g_bb, 'z');
    csilk_bounded_buf_putc(&g_bb, 'w'); /* overflows */
    assert(csilk_bounded_buf_overflow(&g_bb) == 1);
    assert(strcmp(csilk_bounded_buf_str(&g_bb), "xyz") == 0);
}

static void
t_json_basics(void)
{
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_string(&g_jb, "hello");
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "\"hello\"") == 0);
    assert(csilk_bounded_json_overflow(&g_jb) == 0);
    csilk_bounded_buf_reset(&g_jb.buf);
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_int(&g_jb, 42);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "42") == 0);
}

static void
t_json_container(void)
{
    /* Array of ints: [1,2,3] */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_array_open(&g_jb);
    csilk_bounded_json_int(&g_jb, 1);
    csilk_bounded_json_int(&g_jb, 2);
    csilk_bounded_json_int(&g_jb, 3);
    csilk_bounded_json_array_close(&g_jb);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "[1,2,3]") == 0);

    /* Object with key/value: {"name":"Alice"} */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_object_open(&g_jb);
    csilk_bounded_json_key(&g_jb, "name");
    csilk_bounded_json_string(&g_jb, "Alice");
    csilk_bounded_json_object_close(&g_jb);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "{\"name\":\"Alice\"}") == 0);

    /* Mixed types: {"b":[true,null,1.5]} */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_object_open(&g_jb);
    csilk_bounded_json_key(&g_jb, "b");
    csilk_bounded_json_array_open(&g_jb);
    csilk_bounded_json_bool(&g_jb, 1);
    csilk_bounded_json_null(&g_jb);
    csilk_bounded_json_double(&g_jb, 1.5, 1);
    csilk_bounded_json_array_close(&g_jb);
    csilk_bounded_json_object_close(&g_jb);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "{\"b\":[true,null,1.5]}") == 0);
}

static void
t_json_escaping(void)
{
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_string(&g_jb, "say \"hi\" \\ newline\n return\r tab\t ctrl\x01");
    /* build expected manually */
    char expect[256];
    snprintf(
        expect, sizeof(expect), "\"say \\\"hi\\\" \\\\ newline\\n return\\r tab\\t ctrl\\u0001\"");
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), expect) == 0);

    /* NULL string -> null */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_string(&g_jb, NULL);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "null") == 0);
}

static void
t_json_helpers(void)
{
    csilk_bounded_json_status(&g_jb, g_buf, BUFN, "ok");
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "{\"status\":\"ok\"}") == 0);

    csilk_bounded_json_error(&g_jb, g_buf, BUFN, "bad");
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "{\"error\":\"bad\"}") == 0);

    /* uint via JSON value */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_uint(&g_jb, 1234567890123ULL);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "1234567890123") == 0);
}

static void
t_json_leading_comma_open(void)
{
    /* A top-level value followed by array_open writes a leading comma and
     * exercises csilk_bounded_json_str + the array_open comma branch. */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_string(&g_jb, "a");
    csilk_bounded_json_array_open(&g_jb);
    csilk_bounded_json_int(&g_jb, 1);
    csilk_bounded_json_array_close(&g_jb);
    assert(strcmp(csilk_bounded_json_str(&g_jb), "\"a\",[1]") == 0);

    /* Same for object_open. */
    csilk_bounded_json_init(&g_jb, g_buf, BUFN);
    csilk_bounded_json_int(&g_jb, 9);
    csilk_bounded_json_object_open(&g_jb);
    csilk_bounded_json_key(&g_jb, "k");
    csilk_bounded_json_int(&g_jb, 1);
    csilk_bounded_json_object_close(&g_jb);
    assert(strcmp(csilk_bounded_buf_str(&g_jb.buf), "9,{\"k\":1}") == 0);
}

static void
t_json_overflow(void)
{
    char                 buf[8];
    csilk_bounded_json_t j;
    csilk_bounded_json_init(&j, buf, sizeof(buf));
    csilk_bounded_json_string(&j, "a-very-long-string-value");
    assert(csilk_bounded_json_overflow(&j) == 1);
}

int
main(void)
{
    printf("Testing bounded buffer & JSON builder...\n");
    t_init_str_len();
    t_putc_puts();
    t_puti();
    t_putu();
    t_putf();
    t_overflow();
    t_json_basics();
    t_json_container();
    t_json_escaping();
    t_json_helpers();
    t_json_leading_comma_open();
    t_json_overflow();
    printf("test_bounded_buf: ALL PASSED\n");
    return 0;
}