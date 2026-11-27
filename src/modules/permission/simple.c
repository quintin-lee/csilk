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
 *   - All table state is guarded by a single process-lifetime static mutex
 *     (s_rules_mutex), lazily created on first use via csilk_once(). This
 *     makes csilk_perm_simple_clear() safe to call at any time: it only
 *     resets the rule table and never destroys the lock, so the driver can
 *     be re-initialized (csilk_perm_simple_init()) and used again.
 *
 * @copyright MIT License
 */

#include "csilk/core/json/json.h"
#include "csilk/core/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/permission/perm.h"
#include "csilk/csilk.h"

/** @brief Maximum permission rules per simple driver. */
enum { MAX_RULES = 128 };

/** @brief Global rule table.  Populated at startup by
 * csilk_perm_simple_allow().  Protected by s_rules_mutex so the table can be
 * hot-modified while request threads read it. */
static csilk_perm_rule_t rules[MAX_RULES];
/** @brief Current number of rules loaded. */
static int rule_count = 0;
/** @brief Lazily-initialized process-lifetime mutex guarding the rule table.
 * Created via csilk_once() (the once primitive from csilk/core/sync.h) on
 * first access, so every entry point (init / allow / check / clear) can lock
 * it without an external init call. A plain mutex is used (csilk has no
 * rwlock primitive) — the critical section is short (a handful of pointer
 * stores) so the overhead of a write-lock is negligible compared to the O(n)
 * scan in simple_check(). NOTE: must never be destroyed while the driver is
 * registered; it lives for the entire process. */
static csilk_mutex_t s_rules_mutex;
static csilk_once_t  s_rules_mutex_once;

/** @brief Create s_rules_mutex exactly once (thread-safe via csilk_once). */
static void
s_rules_mutex_init_once(void)
{
    csilk_mutex_init(&s_rules_mutex);
}

/** @brief Acquire the rule-table mutex, creating it on first use. */
static void
s_rules_mutex_lock(void)
{
    csilk_once(&s_rules_mutex_once, s_rules_mutex_init_once);
    csilk_mutex_lock(&s_rules_mutex);
}

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

    csilk_json_t* payload = (csilk_json_t*)csilk_get(c, "jwt_payload");
    if (payload) {
        csilk_json_t* r = csilk_json_get(payload, "role");
        if (r && csilk_json_is_string(r)) {
            return csilk_json_string_value(r);
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

    s_rules_mutex_lock();
    int n = rule_count;
    for (int i = 0; i < n; i++) {
        if (match_pattern(rules[i].role, role) && match_pattern(rules[i].permission, permission) &&
            match_pattern(rules[i].resource, resource)) {
            csilk_mutex_unlock(&s_rules_mutex);
            return 0;
        }
    }
    csilk_mutex_unlock(&s_rules_mutex);

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
 * subsystem. The driver registry is idempotent — registering the same
 * driver name twice keeps the original entry, so repeated calls are safe
 * (e.g. re-init after csilk_perm_simple_clear()). The underlying rule-table
 * mutex is created lazily and process-lifetime; it is never destroyed here.
 */
void
csilk_perm_simple_init(void)
{
    s_rules_mutex_lock();
    rule_count = 0;
    csilk_mutex_unlock(&s_rules_mutex);
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
    if (!role || !permission || !resource) {
        return -1;
    }
    s_rules_mutex_lock();
    if (rule_count >= MAX_RULES) {
        csilk_mutex_unlock(&s_rules_mutex);
        return -1;
    }
    rules[rule_count].role = role;
    rules[rule_count].permission = permission;
    rules[rule_count].resource = resource;
    rule_count++;
    csilk_mutex_unlock(&s_rules_mutex);
    return 0;
}

/** @brief Remove all permission rules.
 * Resets the table so that all subsequent checks will be denied.
 * The rule-table mutex is process-lifetime and intentionally NOT destroyed
 * here — destroying it while request threads may still be using the driver
 * is undefined behaviour and is what caused SIGABRT in multi-app test
 * suites (Python integration tests create/destroy several csilk apps in
 * one process). Callers that need a fresh driver must re-run
 * csilk_perm_simple_init(), which simply clears the table and re-registers
 * the driver vtable. */
void
csilk_perm_simple_clear(void)
{
    s_rules_mutex_lock();
    rule_count = 0;
    csilk_mutex_unlock(&s_rules_mutex);
}
