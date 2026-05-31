/**
 * @file waf.c
 * @brief Web Application Firewall (WAF) middleware implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "csilk/core/internal.h"
#include "core/ctx_types.h"
#include "csilk/csilk.h"

/** @brief Common patterns for SQL Injection attacks. */
static const char* sql_patterns[] = {"UNION SELECT",
				     "SELECT FROM",
				     "INSERT INTO",
				     "UPDATE SET",
				     "DELETE FROM",
				     "DROP TABLE",
				     "OR '1'='1",
				     "OR \"1\"=\"1",
				     "WAITFOR DELAY",
				     "SLEEP(",
				     "PG_SLEEP(",
				     nullptr};

/** @brief Common patterns for Cross-Site Scripting (XSS) attacks. */
static const char* xss_patterns[] = {
    "<SCRIPT", "ONERROR=", "ONLOAD=", "JAVASCRIPT:", "ALERT(", nullptr};

/** @brief Common patterns for Directory Traversal attacks. */
static const char* traversal_patterns[] = {"../", "..\\", nullptr};

/**
 * @brief Simple case-insensitive search for a pattern in a string.
 *
 * @param haystack String to search in.
 * @param needle   Pattern to search for (must be uppercase).
 * @return 1 if found, 0 otherwise.
 */
static int
_csilk_strcasestr_pattern(const char* haystack, const char* needle)
{
	if (!haystack || !needle) {
		return 0;
	}
	size_t needle_len = strlen(needle);
	size_t haystack_len = strlen(haystack);

	if (needle_len > haystack_len) {
		return 0;
	}

	for (size_t i = 0; i <= haystack_len - needle_len; i++) {
		size_t j;
		for (j = 0; j < needle_len; j++) {
			if (toupper((unsigned char)haystack[i + j]) != (unsigned char)needle[j]) {
				break;
			}
		}
		if (j == needle_len) {
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Check if a string contains any of the specified patterns.
 *
 * @param input    The string to check.
 * @param patterns Null-terminated array of uppercase pattern strings.
 * @return 1 if any pattern is found, 0 otherwise.
 */
static int
contains_pattern(const char* input, const char** patterns)
{
	if (!input) {
		return 0;
	}
	for (int i = 0; patterns[i]; i++) {
		if (_csilk_strcasestr_pattern(input, patterns[i])) {
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Check all entries in a header map for malicious patterns.
 */
static int
check_map(csilk_header_map_t* map)
{
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = map->buckets[i];
		while (h) {
			if (contains_pattern(h->value, sql_patterns) ||
			    contains_pattern(h->value, xss_patterns) ||
			    contains_pattern(h->value, traversal_patterns)) {
				return 1;
			}
			h = h->next;
		}
	}
	return 0;
}

/**
 * @brief WAF (Web Application Firewall) middleware.
 *
 * Inspects the request path, query parameters, and form parameters for common
 * attack patterns including SQL Injection, XSS, and Directory Traversal.
 *
 * If a malicious pattern is detected, the request is blocked with a
 * 403 Forbidden response.
 *
 * @param c  The request context.
 */
void
csilk_waf_middleware(csilk_ctx_t* c)
{
	int blocked = 0;

	/* Check path for directory traversal */
	if (contains_pattern(c->request.path, traversal_patterns)) {
		blocked = 1;
	}

	/* Check query parameters for SQLi/XSS */
	if (!blocked && check_map(&c->request.query_params)) {
		blocked = 1;
	}

	/* Check form parameters for SQLi/XSS */
	if (!blocked && check_map(&c->request.form_params)) {
		blocked = 1;
	}

	if (blocked) {
		csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Request blocked by WAF");
		csilk_abort(c);
		return;
	}

	csilk_next(c);
}
