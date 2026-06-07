/**
 * @file bounded_buf.c
 * @brief Bounded string buffer and JSON builder implementation.
 *
 * Both layers guarantee zero heap allocation.  On overflow the output is
 * truncated and the overflow flag is set; the caller should fall back to
 * cJSON if the result is too large.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/bounded_buf.h"

/* ===================================================================
 * Bounded string buffer
 * =================================================================== */

void
csilk_bounded_buf_init(csilk_bounded_buf_t* b, char* buf, size_t capacity)
{
	b->buf = buf;
	b->capacity = capacity;
	b->len = 0;
	b->overflow = 0;
	if (capacity > 0) {
		buf[0] = '\0';
	}
}

void
csilk_bounded_buf_reset(csilk_bounded_buf_t* b)
{
	b->len = 0;
	b->overflow = 0;
	if (b->capacity > 0) {
		b->buf[0] = '\0';
	}
}

int
csilk_bounded_buf_overflow(const csilk_bounded_buf_t* b)
{
	return b->overflow;
}

const char*
csilk_bounded_buf_str(const csilk_bounded_buf_t* b)
{
	return b->buf;
}

size_t
csilk_bounded_buf_len(const csilk_bounded_buf_t* b)
{
	return b->len;
}

void
csilk_bounded_buf_putc(csilk_bounded_buf_t* b, char c)
{
	if (b->len + 1 < b->capacity) {
		b->buf[b->len++] = c;
		b->buf[b->len] = '\0';
	} else {
		b->overflow = 1;
	}
}

void
csilk_bounded_buf_puts(csilk_bounded_buf_t* b, const char* s)
{
	if (!s) {
		return;
	}
	while (*s) {
		csilk_bounded_buf_putc(b, *s);
		s++;
	}
}

static char*
uint64_to_str(char* end, uint64_t n)
{
	*end = '\0';
	if (n == 0) {
		*--end = '0';
		return end;
	}
	while (n > 0) {
		*--end = (char)('0' + (n % 10));
		n /= 10;
	}
	return end;
}

void
csilk_bounded_buf_puti(csilk_bounded_buf_t* b, int64_t n)
{
	char tmp[24];
	char* end = tmp + sizeof(tmp);
	if (n < 0) {
		csilk_bounded_buf_putc(b, '-');
		end = uint64_to_str(end, (uint64_t)(-(n + 1)) + 1);
	} else {
		end = uint64_to_str(end, (uint64_t)n);
	}
	csilk_bounded_buf_puts(b, end);
}

void
csilk_bounded_buf_putu(csilk_bounded_buf_t* b, uint64_t n)
{
	char tmp[24];
	char* end = uint64_to_str(tmp + sizeof(tmp), n);
	csilk_bounded_buf_puts(b, end);
}

void
csilk_bounded_buf_putf(csilk_bounded_buf_t* b, double d, int precision)
{
	char tmp[64];
	int n = snprintf(tmp, sizeof(tmp), "%.*f", precision, d);
	if (n > 0) {
		csilk_bounded_buf_puts(b, tmp);
	}
}

/* ===================================================================
 * Bounded JSON builder
 * =================================================================== */

void
csilk_bounded_json_init(csilk_bounded_json_t* j, char* buf, size_t capacity)
{
	csilk_bounded_buf_init(&j->buf, buf, capacity);
	j->comma = 0;
}

int
csilk_bounded_json_overflow(const csilk_bounded_json_t* j)
{
	return csilk_bounded_buf_overflow(&j->buf);
}

const char*
csilk_bounded_json_str(const csilk_bounded_json_t* j)
{
	return csilk_bounded_buf_str(&j->buf);
}

/* --- Helper: write a JSON-escaped string value --- */

static void
json_write_escaped(csilk_bounded_json_t* j, const char* s)
{
	if (!s) {
		csilk_bounded_buf_puts(&j->buf, "null");
		return;
	}
	csilk_bounded_buf_putc(&j->buf, '"');
	while (*s) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"':
			csilk_bounded_buf_puts(&j->buf, "\\\"");
			break;
		case '\\':
			csilk_bounded_buf_puts(&j->buf, "\\\\");
			break;
		case '\n':
			csilk_bounded_buf_puts(&j->buf, "\\n");
			break;
		case '\r':
			csilk_bounded_buf_puts(&j->buf, "\\r");
			break;
		case '\t':
			csilk_bounded_buf_puts(&j->buf, "\\t");
			break;
		default:
			if (c < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				csilk_bounded_buf_puts(&j->buf, esc);
			} else {
				csilk_bounded_buf_putc(&j->buf, (char)c);
			}
			break;
		}
		s++;
	}
	csilk_bounded_buf_putc(&j->buf, '"');
}

/* --- Comma separator --- */

static void
json_write_comma(csilk_bounded_json_t* j)
{
	if (j->comma) {
		csilk_bounded_buf_putc(&j->buf, ',');
	}
	j->comma = 1;
}

/* --- Object / Array --- */

void
csilk_bounded_json_object_open(csilk_bounded_json_t* j)
{
	if (j->comma) {
		csilk_bounded_buf_putc(&j->buf, ',');
	}
	csilk_bounded_buf_putc(&j->buf, '{');
	j->comma = 0;
}

void
csilk_bounded_json_object_close(csilk_bounded_json_t* j)
{
	csilk_bounded_buf_putc(&j->buf, '}');
	j->comma = 1;
}

void
csilk_bounded_json_array_open(csilk_bounded_json_t* j)
{
	if (j->comma) {
		csilk_bounded_buf_putc(&j->buf, ',');
	}
	csilk_bounded_buf_putc(&j->buf, '[');
	j->comma = 0;
}

void
csilk_bounded_json_array_close(csilk_bounded_json_t* j)
{
	csilk_bounded_buf_putc(&j->buf, ']');
	j->comma = 1;
}

/* --- Values --- */

void
csilk_bounded_json_key(csilk_bounded_json_t* j, const char* key)
{
	json_write_comma(j);
	json_write_escaped(j, key);
	csilk_bounded_buf_putc(&j->buf, ':');
	j->comma = 0;
}

void
csilk_bounded_json_string(csilk_bounded_json_t* j, const char* s)
{
	json_write_comma(j);
	json_write_escaped(j, s);
}

void
csilk_bounded_json_int(csilk_bounded_json_t* j, int64_t n)
{
	json_write_comma(j);
	csilk_bounded_buf_puti(&j->buf, n);
}

void
csilk_bounded_json_uint(csilk_bounded_json_t* j, uint64_t n)
{
	json_write_comma(j);
	csilk_bounded_buf_putu(&j->buf, n);
}

void
csilk_bounded_json_double(csilk_bounded_json_t* j, double d, int precision)
{
	json_write_comma(j);
	csilk_bounded_buf_putf(&j->buf, d, precision);
}

void
csilk_bounded_json_bool(csilk_bounded_json_t* j, int v)
{
	json_write_comma(j);
	csilk_bounded_buf_puts(&j->buf, v ? "true" : "false");
}

void
csilk_bounded_json_null(csilk_bounded_json_t* j)
{
	json_write_comma(j);
	csilk_bounded_buf_puts(&j->buf, "null");
}

/* --- Convenience helpers --- */

void
csilk_bounded_json_status(csilk_bounded_json_t* j, char* buf, size_t capacity, const char* status)
{
	csilk_bounded_json_init(j, buf, capacity);
	csilk_bounded_json_object_open(j);
	csilk_bounded_json_key(j, "status");
	csilk_bounded_json_string(j, status);
	csilk_bounded_json_object_close(j);
}

void
csilk_bounded_json_error(csilk_bounded_json_t* j, char* buf, size_t capacity, const char* message)
{
	csilk_bounded_json_init(j, buf, capacity);
	csilk_bounded_json_object_open(j);
	csilk_bounded_json_key(j, "error");
	csilk_bounded_json_string(j, message);
	csilk_bounded_json_object_close(j);
}
