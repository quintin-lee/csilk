/**
 * @file waf.c
 * @brief Web Application Firewall (WAF) middleware implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

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
 * @brief Callback for header/parameter iteration to check for malicious
 * patterns.
 */
static int
check_pattern_cb(const char* key, const char* value, void* arg)
{
	(void)key;
	int* blocked = (int*)arg;
	if (contains_pattern(value, sql_patterns) || contains_pattern(value, xss_patterns) ||
	    contains_pattern(value, traversal_patterns)) {
		*blocked = 1;
		return 0; /* Stop iteration */
	}
	return 1; /* Continue */
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

	if (!c) {
		return;
	}

	/* Check path for directory traversal */
	if (contains_pattern(csilk_get_path(c), traversal_patterns)) {
		blocked = 1;
	}

	/* Check query parameters for SQLi/XSS */
	if (!blocked) {
		csilk_for_each_query(c, check_pattern_cb, &blocked);
	}

	/* Check form parameters for SQLi/XSS */
	if (!blocked) {
		csilk_for_each_form_field(c, check_pattern_cb, &blocked);
	}

	if (blocked) {
		csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Request blocked by WAF");
		csilk_abort(c);
		return;
	}

	csilk_next(c);
}
