/**
 * @file waf.c
 * @brief Web Application Firewall (WAF) middleware implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "csilk/csilk.h"

typedef struct {
    const char* pattern;
    size_t      len;
} waf_pattern_t;

/** @brief Common patterns for SQL Injection attacks. */
static waf_pattern_t sql_rules[] = {
    {"UNION SELECT",  sizeof("UNION SELECT") - 1 },
    {"SELECT FROM",   sizeof("SELECT FROM") - 1  },
    {"INSERT INTO",   sizeof("INSERT INTO") - 1  },
    {"UPDATE SET",    sizeof("UPDATE SET") - 1   },
    {"DELETE FROM",   sizeof("DELETE FROM") - 1  },
    {"DROP TABLE",    sizeof("DROP TABLE") - 1   },
    {"OR '1'='1",     sizeof("OR '1'='1") - 1    },
    {"OR \"1\"=\"1",  sizeof("OR \"1\"=\"1") - 1 },
    {"WAITFOR DELAY", sizeof("WAITFOR DELAY") - 1},
    {"SLEEP(",        sizeof("SLEEP(") - 1       },
    {"PG_SLEEP(",     sizeof("PG_SLEEP(") - 1    },
    {nullptr,         0                          }
};

/** @brief Common patterns for Cross-Site Scripting (XSS) attacks. */
static waf_pattern_t xss_rules[] = {
    {"<SCRIPT",     sizeof("<SCRIPT") - 1    },
    {"ONERROR=",    sizeof("ONERROR=") - 1   },
    {"ONLOAD=",     sizeof("ONLOAD=") - 1    },
    {"JAVASCRIPT:", sizeof("JAVASCRIPT:") - 1},
    {"ALERT(",      sizeof("ALERT(") - 1     },
    {nullptr,       0                        }
};

/** @brief Common patterns for Directory Traversal attacks. */
static waf_pattern_t traversal_rules[] = {
    {"../",   sizeof("../") - 1 },
    {"..\\",  sizeof("..\\") - 1},
    {nullptr, 0                 }
};

/**
 * @brief Simple case-insensitive search for a pattern in a string.
 *
 * @param haystack String to search in.
 * @param needle   Pattern to search for (must be uppercase).
 * @param needle_len Length of the pattern.
 * @return 1 if found, 0 otherwise.
 */
static int
_csilk_strcasestr_pattern(const char* haystack, const char* needle, size_t needle_len)
{
    if (!haystack || !needle) {
        return 0;
    }
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

typedef struct {
    csilk_ctx_t* c;
    int          blocked;
    const char*  matched_key;
    const char*  matched_val;
    const char*  matched_pattern;
    const char*  attack_type;
} waf_check_ctx_t;

/**
 * @brief Callback for header/parameter iteration to check for malicious
 * patterns.
 */
static int
check_pattern_cb(const char* key, const char* value, void* arg)
{
    waf_check_ctx_t* wctx = (waf_check_ctx_t*)arg;

    /* Check SQL Injection */
    for (int i = 0; sql_rules[i].pattern; i++) {
        if (_csilk_strcasestr_pattern(value, sql_rules[i].pattern, sql_rules[i].len)) {
            wctx->blocked = 1;
            wctx->matched_key = key;
            wctx->matched_val = value;
            wctx->matched_pattern = sql_rules[i].pattern;
            wctx->attack_type = "SQL Injection";
            return 0; /* Stop iteration */
        }
    }

    /* Check XSS */
    for (int i = 0; xss_rules[i].pattern; i++) {
        if (_csilk_strcasestr_pattern(value, xss_rules[i].pattern, xss_rules[i].len)) {
            wctx->blocked = 1;
            wctx->matched_key = key;
            wctx->matched_val = value;
            wctx->matched_pattern = xss_rules[i].pattern;
            wctx->attack_type = "XSS";
            return 0; /* Stop iteration */
        }
    }

    /* Check Directory Traversal */
    for (int i = 0; traversal_rules[i].pattern; i++) {
        if (_csilk_strcasestr_pattern(value, traversal_rules[i].pattern, traversal_rules[i].len)) {
            wctx->blocked = 1;
            wctx->matched_key = key;
            wctx->matched_val = value;
            wctx->matched_pattern = traversal_rules[i].pattern;
            wctx->attack_type = "Directory Traversal";
            return 0; /* Stop iteration */
        }
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

    const char* path = csilk_get_path(c);
    CSILK_LOG_T(
        "WAF: Middleware inspecting request %p (path: '%s')", (void*)c, path ? path : "none");

    /* Check path for directory traversal */
    for (int i = 0; traversal_rules[i].pattern; i++) {
        if (_csilk_strcasestr_pattern(path, traversal_rules[i].pattern, traversal_rules[i].len)) {
            CSILK_LOG_W("WAF: Blocked request %p: Directory Traversal attack detected "
                        "in path '%s' (pattern: '%s')",
                        (void*)c,
                        path ? path : "",
                        traversal_rules[i].pattern);
            blocked = 1;
            break;
        }
    }

    waf_check_ctx_t wctx = {c, 0, nullptr, nullptr, nullptr, nullptr};

    /* Check query parameters for SQLi/XSS */
    if (!blocked) {
        csilk_for_each_query(c, check_pattern_cb, &wctx);
        if (wctx.blocked) {
            CSILK_LOG_W("WAF: Blocked request %p: %s attack detected in query "
                        "parameter '%s' (value: '%s', pattern: '%s')",
                        (void*)c,
                        wctx.attack_type,
                        wctx.matched_key ? wctx.matched_key : "",
                        wctx.matched_val ? wctx.matched_val : "",
                        wctx.matched_pattern ? wctx.matched_pattern : "");
            blocked = 1;
        }
    }

    /* Check form parameters for SQLi/XSS */
    if (!blocked) {
        csilk_for_each_form_field(c, check_pattern_cb, &wctx);
        if (wctx.blocked) {
            CSILK_LOG_W("WAF: Blocked request %p: %s attack detected in form field "
                        "'%s' (value: '%s', pattern: '%s')",
                        (void*)c,
                        wctx.attack_type,
                        wctx.matched_key ? wctx.matched_key : "",
                        wctx.matched_val ? wctx.matched_val : "",
                        wctx.matched_pattern ? wctx.matched_pattern : "");
            blocked = 1;
        }
    }

    if (blocked) {
        csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Request blocked by WAF");
        csilk_abort(c);
        return;
    }

    csilk_next(c);
}
