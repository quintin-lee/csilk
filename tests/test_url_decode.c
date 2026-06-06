#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"

void
test_basic_decode()
{
	char buf[] = "%48%65%6C%6C%6F";
	size_t len = csilk_url_decode(buf);
	assert(len == 5);
	assert(strcmp(buf, "Hello") == 0);
	printf("test_basic_decode passed\n");
}

void
test_plus_to_space()
{
	char buf[] = "hello+world";
	size_t len = csilk_url_decode(buf);
	assert(len == 11);
	assert(strcmp(buf, "hello world") == 0);
	printf("test_plus_to_space passed\n");
}

void
test_mixed()
{
	char buf[] = "%48ello+World%21";
	size_t len = csilk_url_decode(buf);
	assert(len == 12);
	assert(strcmp(buf, "Hello World!") == 0);
	printf("test_mixed passed\n");
}

void
test_no_encoding()
{
	char buf[] = "plain text";
	size_t len = csilk_url_decode(buf);
	assert(len == 10);
	assert(strcmp(buf, "plain text") == 0);
	printf("test_no_encoding passed\n");
}

void
test_empty()
{
	char buf[] = "";
	size_t len = csilk_url_decode(buf);
	assert(len == 0);
	assert(strcmp(buf, "") == 0);
	printf("test_empty passed\n");
}

void
test_null_input()
{
	size_t len = csilk_url_decode(nullptr);
	assert(len == 0);
	printf("test_null_input passed\n");
}

void
test_invalid_hex()
{
	char buf[] = "%XX%GG%1";
	size_t len = csilk_url_decode(buf);
	assert(len == 8);
	assert(strcmp(buf, "%XX%GG%1") == 0);
	printf("test_invalid_hex passed\n");
}

void
test_lowercase_hex()
{
	char buf[] = "%2f%7e";
	size_t len = csilk_url_decode(buf);
	assert(len == 2);
	assert(strcmp(buf, "/~") == 0);
	printf("test_lowercase_hex passed\n");
}

void
test_uppercase_hex()
{
	char buf[] = "%2F%7E";
	size_t len = csilk_url_decode(buf);
	assert(len == 2);
	assert(strcmp(buf, "/~") == 0);
	printf("test_uppercase_hex passed\n");
}

void
test_percent_only()
{
	char buf[] = "%";
	size_t len = csilk_url_decode(buf);
	assert(len == 1);
	assert(strcmp(buf, "%") == 0);
	printf("test_percent_only passed\n");
}

void
test_percent_one_char()
{
	char buf[] = "%4";
	size_t len = csilk_url_decode(buf);
	assert(len == 2);
	assert(strcmp(buf, "%4") == 0);
	printf("test_percent_one_char passed\n");
}

void
test_special_chars()
{
	char buf[] = "%00%01%1F";
	size_t len = csilk_url_decode(buf);
	assert(len == 3);
	assert((unsigned char)buf[0] == 0x00);
	assert((unsigned char)buf[1] == 0x01);
	assert((unsigned char)buf[2] == 0x1F);
	assert(buf[3] == '\0');
	printf("test_special_chars passed\n");
}

int
main()
{
	test_basic_decode();
	test_plus_to_space();
	test_mixed();
	test_no_encoding();
	test_empty();
	test_null_input();
	test_invalid_hex();
	test_lowercase_hex();
	test_uppercase_hex();
	test_percent_only();
	test_percent_one_char();
	test_special_chars();
	printf("test_url_decode: ALL PASSED\n");
	return 0;
}
