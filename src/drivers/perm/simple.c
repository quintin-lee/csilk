/**
 * @file perm_simple.c
 * @brief Simple rule-based permission driver for csilk.
 *
 * Implements the csilk_perm_driver_t vtable using an in-memory rule table.
 * Each rule is a (role, permission, resource) triple.  A check succeeds if
 * a rule matches all three fields (wildcards supported).
 *
 * Key design points:
 *   - Rules are stored in a fixed-size array (MAX_RULES = 128).
 *   - Pattern matching supports exact match, global wildcard "*", and
 *     prefix wildcard "prefix:*".
 *   - The role is resolved from the request context (either a "role" key
 *     or the "jwt_payload" JSON object's "role" field).
 *
 * @copyright MIT License
 */

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/perm.h"
#include "csilk/core/internal.h"

/** @brief Global rule table.  Populated at startup by
 * csilk_perm_simple_allow(). */
static csilk_perm_rule_t rules[MAX_RULES];
/** @brief Current number of rules loaded. */
static int rule_count = 0;

/**
 * @brief Match a pattern string against a value.
 *
 * Supports three matching modes:
 *   1. Global wildcard: pattern "*" matches anything.
 *   2. Prefix wildcard: pattern "prefix:*" matches any value starting with
 *      "prefix:".
 *   3. Exact match: otherwise requires strcmp equality.
 *
 * @param pattern  Pattern string (may include wildcards).
 * @param value    Concrete value to test.
 * @return 1 if the value matches the pattern, 0 otherwise.
 */
static int
match_pattern(const char* pattern, const char* value)
{
	if (!pattern || !value) {
		return 0;
	}
	if (strcmp(pattern, "*") == 0) {
		return 1;
	}
	size_t plen = strlen(pattern);
	if (plen > 0 && pattern[plen - 1] == '*') {
		size_t pfxlen = plen - 1;
		if (pfxlen > 0 && pattern[pfxlen - 1] == ':') {
			/* Prefix wildcard: "namespace:*" matches everything under that prefix */
			return strncmp(pattern, value, pfxlen) == 0;
		}
	}
	return strcmp(pattern, value) == 0;
}

/**
 * @brief Resolve the current user's role from the request context.
 *
 * Checks two sources in order:
 *   1. The "role" key directly on the context.
 *   2. The "role" field inside the "jwt_payload" JSON object.
 *
 * @param c  The current request context.
 * @return A pointer to the role string (borrowed from context), or NULL if
 *         no role can be determined.
 */
static const char*
get_role_from_ctx(csilk_ctx_t* c)
{
	const char* role = (const char*)csilk_get(c, "role");
	if (role) {
		return role;
	}

	cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
	if (payload) {
		cJSON* r = cJSON_GetObjectItem(payload, "role");
		if (r && cJSON_IsString(r)) {
			return r->valuestring;
		}
	}

	return NULL;
}

/**
 * @brief Check whether the current request has a given permission on a
 *        resource.
 *
 * Resolves the role from the context, then linearly scans the rule table.
 * A match requires all three fields (role, permission, resource) to match
 * according to match_pattern() semantics.
 *
 * @param c          Request context.
 * @param permission The permission to check (e.g., "read", "write").
 * @param resource   The resource identifier (e.g., "document:42").
 * @return 0 if permitted, -1 if denied or role cannot be determined.
 */
static int
simple_check(csilk_ctx_t* c, const char* permission, const char* resource)
{
	const char* role = get_role_from_ctx(c);
	if (!role) {
		return -1;
	}

	for (int i = 0; i < rule_count; i++) {
		if (match_pattern(rules[i].role, role) &&
		    match_pattern(rules[i].permission, permission) &&
		    match_pattern(rules[i].resource, resource)) {
			return 0;
		}
	}

	return -1;
}

/** @brief Driver vtable for the simple rule-based permission backend. */
csilk_perm_driver_t csilk_perm_simple_driver = {
    .name = "simple",
    .check = simple_check,
};

/**
 * @brief Initialise and register the simple permission driver.
 * Clears any existing rules and makes "simple" available to the permission
 * subsystem.
 */
void
csilk_perm_simple_init(void)
{
	rule_count = 0;
	csilk_perm_register_driver("simple", &csilk_perm_simple_driver);
}

/**
 * @brief Add an allow rule to the permission table.
 *
 * @param role       Role identifier (may contain wildcards).
 * @param permission Permission name (may contain wildcards).
 * @param resource   Resource pattern (may contain wildcards).
 * @return 0 on success, -1 if the table is full or parameters are NULL.
 */
int
csilk_perm_simple_allow(const char* role, const char* permission, const char* resource)
{
	if (!role || !permission || !resource || rule_count >= MAX_RULES) {
		return -1;
	}
	rules[rule_count].role = role;
	rules[rule_count].permission = permission;
	rules[rule_count].resource = resource;
	rule_count++;
	return 0;
}

/**
 * @brief Remove all permission rules.
 * Resets the table so that all subsequent checks will be denied.
 */
void
csilk_perm_simple_clear(void)
{
	rule_count = 0;
}
