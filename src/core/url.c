/**
 * @file url.c
 * @brief URL path splitting and percent-decoding utilities.
 *
 * Provides the two fundamental URL operations needed by the HTTP server:
 *   1. csilk_url_decode()   — in-place percent-decoding (%XX -> byte, '+' ->
 * space)
 *   2. csilk_split_url()    — separates "path?query" into decoded path + raw
 * query
 *
 * These are used by the HTTP parser during request finalization to populate
 * the request context's path and query_params fields.
 * @copyright MIT License
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Helper: convert a hexadecimal character to its integer value.
 *
 * Handles both uppercase ('A'-'F') and lowercase ('a'-'f') hex digits.
 *
 * @param c Hex character ('0'-'9', 'a'-'f', or 'A'-'F').
 * @return Integer value 0-15, or -1 if @p c is not a valid hex digit. */
static int
hex_to_int(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/** @brief URL-decode a percent-encoded string in-place.
 *
 * Replaces %XX sequences with the corresponding byte value and '+' with
 * space. The decoding is done in-place so the output is never longer than
 * the input.
 *
 * @param str Null-terminated string to decode (modified in-place).
 * @return The length of the decoded string (may be shorter than original).
 * @note If @p str contains invalid % sequences (e.g., "%ZZ"), they are left
 *       as-is. */
size_t
csilk_url_decode(char* str)
{
	if (!str) {
		return 0;
	}
	char* src = str;
	char* dst = str;
	while (*src) {
		if (*src == '%' && isxdigit(src[1]) && isxdigit(src[2])) {
			int hi = hex_to_int(src[1]);
			int lo = hex_to_int(src[2]);
			if (hi >= 0 && lo >= 0) {
				*dst++ = (char)((hi << 4) | lo);
				src += 3;
				continue;
			}
		} else if (*src == '+') {
			*dst++ = ' ';
		} else {
			*dst++ = *src;
		}
		src++;
	}
	*dst = '\0';
	return dst - str;
}

/** @brief Split a full URL into its path and query string components.
 *
 * Finds the first '?' separator. The path portion is URL-decoded and
 * returned in @p path. The query portion (everything after '?') is
 * returned raw in @p query (NOT URL-decoded — use csilk_parse_query()
 * for that). If there is no '?', the entire URL is treated as the path
 * and @p query is set to NULL.
 *
 * @param url   Full URL string (e.g., "/foo/bar?key=val").
 * @param path  [out] Receives a malloc'd, URL-decoded path string.
 * @param query [out] Receives a malloc'd raw query string, or NULL if no
 *              query was present.
 * @note Both output strings must be freed by the caller with free().
 *       On allocation failure, both outputs may be NULL. */
void
csilk_split_url(const char* url, char** path, char** query)
{
	*path = NULL;
	*query = NULL;
	if (!url) {
		return;
	}

	const char* qmark = strchr(url, '?');
	if (qmark) {
		size_t path_len = qmark - url;
		*path = malloc(path_len + 1);
		if (!*path) {
			return;
		}

		memcpy(*path, url, path_len);
		(*path)[path_len] = '\0';
		csilk_url_decode(*path);

		*query = strdup(qmark + 1);
		if (!*query) {
			free(*path);
			*path = NULL;
		}
	} else {
		*path = strdup(url);
		if (*path) {
			csilk_url_decode(*path);
		}
	}
}
