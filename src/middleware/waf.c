/**
 * @file waf.c
 * @brief Web Application Firewall (WAF) middleware implementation.
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

typedef struct {
    const char* pattern;
    size_t      len;
} waf_pattern_t;

typedef struct {
    uint32_t ip;
    uint32_t prefix_len;
    uint8_t  action;
} waf_ip_rule_t;

struct csilk_waf_filter_s {
    waf_ip_rule_t rules[256];
    size_t        rule_count;
    char*         patterns[128];
    size_t        pattern_count;
    csilk_mutex_t mutex;
};

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
    {NULL,            0                          }
};

/** @brief Common patterns for Cross-Site Scripting (XSS) attacks. */
static waf_pattern_t xss_rules[] = {
    {"<SCRIPT",     sizeof("<SCRIPT") - 1    },
    {"ONERROR=",    sizeof("ONERROR=") - 1   },
    {"ONLOAD=",     sizeof("ONLOAD=") - 1    },
    {"JAVASCRIPT:", sizeof("JAVASCRIPT:") - 1},
    {"ALERT(",      sizeof("ALERT(") - 1     },
    {NULL,          0                        }
};

/** @brief Common patterns for Directory Traversal attacks. */
static waf_pattern_t traversal_rules[] = {
    {"../",  sizeof("../") - 1 },
    {"..\\", sizeof("..\\") - 1},
    {NULL,   0                 }
};

/* Per-request scan budget (bytes).  Bodies larger than this are skipped
 * to prevent CPU-exhaustion DoS via oversized payloads. */
#define CSILK_WAF_MAX_BODY_SCAN (256 * 1024)
#define CSILK_WAF_MAX_VALUE_SCAN (64 * 1024)

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

/**
 * @brief Rabin-Karp accelerated case-insensitive pattern search.
 *
 * Pre-computes a rolling hash for each pattern, then slides a window
 * over the haystack in O(n) per pattern instead of the naive O(n*m).
 * For small haystacks (≤ 256 bytes) falls back to the naive scan which
 * avoids the hash-table overhead.
 */
static int
_csilk_rabin_karp_scan(const char*          haystack,
                       size_t               haystack_len,
                       const waf_pattern_t* rules,
                       int                  rule_count)
{
    if (!haystack || haystack_len == 0 || rule_count <= 0) {
        return 0;
    }

    /* Naive path for very short inputs — cheaper than building hash tables. */
    if (haystack_len <= 256) {
        for (int r = 0; r < rule_count; r++) {
            size_t pl = rules[r].len;
            if (pl > haystack_len) {
                continue;
            }
            for (size_t i = 0; i + pl <= haystack_len; i++) {
                int match = 1;
                for (size_t j = 0; j < pl; j++) {
                    if (toupper((unsigned char)haystack[i + j]) !=
                        (unsigned char)rules[r].pattern[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    return 1;
                }
            }
        }
        return 0;
    }

    /* Rolling-hash path: use 64-bit arithmetic so (val * 256 + byte) % MOD
     * never overflows uint32_t on large payloads (>256 bytes). */
    const uint64_t BK = 256ULL;         /* alphabet for upper-cased chars */
    const uint64_t MOD = 1000000007ULL; /* large prime */

    /* Pre-compute pattern hashes (upper-cased). */
    uint64_t* pat_hashes = (uint64_t*)calloc((size_t)rule_count, sizeof(uint64_t));
    uint64_t* pat_pows = (uint64_t*)calloc((size_t)rule_count, sizeof(uint64_t));
    size_t    max_plen = 0;
    if (!pat_hashes || !pat_pows) {
        free(pat_hashes);
        free(pat_pows);
        return 0;
    }
    for (int r = 0; r < rule_count; r++) {
        size_t pl = rules[r].len;
        if (pl > haystack_len) {
            continue;
        }
        if (pl > max_plen) {
            max_plen = pl;
        }
        /* Compute hash of upper-cased pattern */
        uint64_t h = 0;
        for (size_t j = 0; j < pl; j++) {
            h = (h * BK + (uint64_t)(uint8_t)toupper((unsigned char)rules[r].pattern[j])) % MOD;
        }
        pat_hashes[r] = h;
        /* precompute BK^(pl-1) mod MOD */
        uint64_t p = 1;
        for (size_t j = 0; j < pl - 1; j++) {
            p = (p * BK) % MOD;
        }
        pat_pows[r] = p;
    }
    if (max_plen == 0) {
        free(pat_hashes);
        free(pat_pows);
        return 0;
    }

    /* Slide a window of max_plen over the upper-cased haystack. */
    uint8_t* upper = (uint8_t*)malloc(haystack_len);
    if (!upper) {
        free(pat_hashes);
        free(pat_pows);
        return 0;
    }
    for (size_t i = 0; i < haystack_len; i++) {
        upper[i] = (uint8_t)toupper((unsigned char)haystack[i]);
    }

    /* For each distinct window length that matches a pattern length, scan. */
    int found = 0;
    for (size_t wlen = 1; wlen <= max_plen && !found; wlen++) {
        int any_rule_this_len = 0;
        for (int r = 0; r < rule_count; r++) {
            if (rules[r].len == wlen) {
                any_rule_this_len = 1;
                break;
            }
        }
        if (!any_rule_this_len) {
            continue;
        }

        uint64_t bk_pow = 1;
        for (size_t j = 0; j < wlen - 1; j++) {
            bk_pow = (bk_pow * BK) % MOD;
        }

        uint64_t win_hash = 0;
        for (size_t j = 0; j < wlen; j++) {
            win_hash = (win_hash * BK + (uint64_t)upper[j]) % MOD;
        }

        /* Check if win_hash matches any pattern of this length. */
        int candidate = 0;
        for (int r = 0; r < rule_count; r++) {
            if (rules[r].len == wlen && pat_hashes[r] == win_hash) {
                candidate = 1;
                break;
            }
        }
        if (candidate) {
            /* Verify actual match at position 0 */
            for (int r = 0; r < rule_count; r++) {
                if (rules[r].len != wlen) {
                    continue;
                }
                int match = 1;
                for (size_t j = 0; j < wlen; j++) {
                    if (upper[j] != (uint8_t)rules[r].pattern[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }

        for (size_t i = wlen; i < haystack_len && !found; i++) {
            uint64_t drop = ((uint64_t)upper[i - wlen] * bk_pow) % MOD;
            win_hash = (win_hash + MOD - drop) % MOD;
            win_hash = (win_hash * BK + (uint64_t)upper[i]) % MOD;

            candidate = 0;
            for (int r = 0; r < rule_count; r++) {
                if (rules[r].len == wlen && pat_hashes[r] == win_hash) {
                    candidate = 1;
                    break;
                }
            }
            if (candidate) {
                for (int r = 0; r < rule_count; r++) {
                    if (rules[r].len != wlen) {
                        continue;
                    }
                    int match = 1;
                    for (size_t j = 0; j < wlen; j++) {
                        if (upper[i - wlen + 1 + j] != (uint8_t)rules[r].pattern[j]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
    }

    free(upper);
    free(pat_hashes);
    free(pat_pows);
    return found;
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
    {
        size_t val_len = strlen(value);
        if (val_len <= CSILK_WAF_MAX_VALUE_SCAN) {
            int sql_count = 0;
            (void)sql_count;
            /* Count rules dynamically */
            int sql_total = 0;
            for (int i = 0; sql_rules[i].pattern; i++) {
                sql_total++;
            }
            if (_csilk_rabin_karp_scan(value, val_len, sql_rules, sql_total)) {
                wctx->blocked = 1;
                wctx->matched_key = key;
                wctx->matched_val = value;
                wctx->matched_pattern = sql_rules[0].pattern; /* first SQL rule label */
                wctx->attack_type = "SQL Injection";
                return 0;
            }
        }
    }

    /* Check XSS */
    {
        size_t val_len = strlen(value);
        if (val_len <= CSILK_WAF_MAX_VALUE_SCAN) {
            int xss_total = 0;
            for (int i = 0; xss_rules[i].pattern; i++) {
                xss_total++;
            }
            if (_csilk_rabin_karp_scan(value, val_len, xss_rules, xss_total)) {
                wctx->blocked = 1;
                wctx->matched_key = key;
                wctx->matched_val = value;
                wctx->matched_pattern = xss_rules[0].pattern;
                wctx->attack_type = "XSS";
                return 0;
            }
        }
    }

    /* Check Directory Traversal */
    {
        size_t val_len = strlen(value);
        if (val_len <= CSILK_WAF_MAX_VALUE_SCAN) {
            int trav_total = 0;
            for (int i = 0; traversal_rules[i].pattern; i++) {
                trav_total++;
            }
            if (_csilk_rabin_karp_scan(value, val_len, traversal_rules, trav_total)) {
                wctx->blocked = 1;
                wctx->matched_key = key;
                wctx->matched_val = value;
                wctx->matched_pattern = traversal_rules[0].pattern;
                wctx->attack_type = "Directory Traversal";
                return 0;
            }
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
    {
        size_t path_len = path ? strlen(path) : 0;
        int    trav_total = 0;
        for (int i = 0; traversal_rules[i].pattern; i++) {
            trav_total++;
        }
        if (path_len > 0 && _csilk_rabin_karp_scan(path, path_len, traversal_rules, trav_total)) {
            CSILK_LOG_W("WAF: Blocked request %p: Directory Traversal attack detected "
                        "in path '%s' (pattern: '%s')",
                        (void*)c,
                        path,
                        traversal_rules[0].pattern);
            blocked = 1;
        }
    }

    waf_check_ctx_t wctx = {c, 0, NULL, NULL, NULL, NULL};

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

    /* Check request body for SQLi/XSS/Traversal attacks.
     * Cap the scan at CSILK_WAF_MAX_BODY_SCAN bytes to prevent CPU-exhaustion
     * DoS via oversized bodies. */
    if (!blocked) {
        const char* body = csilk_get_body(c, NULL);
        size_t      body_len = csilk_get_body_len(c);
        if (body && body_len > 0) {
            size_t scan_len =
                body_len < CSILK_WAF_MAX_BODY_SCAN ? body_len : CSILK_WAF_MAX_BODY_SCAN;
            wctx.blocked = 0;
            /* Inline Rabin-Karp scan over all three rule sets (strictly length-bounded). */
            int sql_total = 0, xss_total = 0, trav_total = 0;
            for (int i = 0; sql_rules[i].pattern; i++) {
                sql_total++;
            }
            for (int i = 0; xss_rules[i].pattern; i++) {
                xss_total++;
            }
            for (int i = 0; traversal_rules[i].pattern; i++) {
                trav_total++;
            }

            if (_csilk_rabin_karp_scan(body, scan_len, sql_rules, sql_total)) {
                wctx.blocked = 1;
                wctx.attack_type = "SQL Injection";
            } else if (_csilk_rabin_karp_scan(body, scan_len, xss_rules, xss_total)) {
                wctx.blocked = 1;
                wctx.attack_type = "XSS";
            } else if (_csilk_rabin_karp_scan(body, scan_len, traversal_rules, trav_total)) {
                wctx.blocked = 1;
                wctx.attack_type = "Directory Traversal";
            }
            if (wctx.blocked) {
                CSILK_LOG_W("WAF: Blocked request %p: %s attack detected in body "
                            "(scanned %zu of %zu bytes)",
                            (void*)c,
                            wctx.attack_type,
                            scan_len,
                            body_len);
                blocked = 1;
            }
        }
    }

    if (blocked) {
        csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Request blocked by WAF");
        csilk_abort(c);
        return;
    }

    csilk_next(c);
}

/* --- Stateful IP/CIDR & Payload WAF Filter --- */

static const void*
_csilk_waf_memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen)
{
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) {
        return NULL;
    }
    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0) {
            return h + i;
        }
    }
    return NULL;
}

csilk_waf_filter_t*
csilk_waf_filter_new(void)
{
    csilk_waf_filter_t* filter = calloc(1, sizeof(csilk_waf_filter_t));
    if (!filter) {
        return NULL;
    }
    csilk_mutex_init(&filter->mutex);
    return filter;
}

int
csilk_waf_filter_add_ip_rule(csilk_waf_filter_t* filter, const char* ip_cidr, uint8_t action)
{
    if (!filter || !ip_cidr) {
        return -1;
    }

    csilk_mutex_lock(&filter->mutex);

    if (filter->rule_count >= 256) {
        csilk_mutex_unlock(&filter->mutex);
        return -1;
    }

    char ip_str[64];
    snprintf(ip_str, sizeof(ip_str), "%s", ip_cidr);
    char*    slash = strchr(ip_str, '/');
    uint32_t prefix_len = 32;

    if (slash) {
        *slash = '\0';
        char* endptr = NULL;
        long  parsed = strtol(slash + 1, &endptr, 10);
        if (endptr == slash + 1 || *endptr != '\0' || parsed < 0 || parsed > 32) {
            csilk_mutex_unlock(&filter->mutex);
            CSILK_LOG_E("WAF Filter: Invalid IPv4 prefix length: %ld (must be 0-32)", parsed);
            return -1;
        }
        prefix_len = (uint32_t)parsed;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        csilk_mutex_unlock(&filter->mutex);
        return -1;
    }

    waf_ip_rule_t* rule = &filter->rules[filter->rule_count++];
    rule->ip = ntohl(addr.s_addr);
    rule->prefix_len = prefix_len;
    rule->action = action;

    csilk_mutex_unlock(&filter->mutex);
    return 0;
}

int
csilk_waf_filter_block_ip(csilk_waf_filter_t* filter, const char* ip_str)
{
    return csilk_waf_filter_add_ip_rule(filter, ip_str, CSILK_WAF_ACTION_DROP);
}

int
csilk_waf_filter_block_pattern(csilk_waf_filter_t* filter, const char* pattern)
{
    if (!filter || !pattern) {
        return -1;
    }

    csilk_mutex_lock(&filter->mutex);
    if (filter->pattern_count >= 128) {
        csilk_mutex_unlock(&filter->mutex);
        return -1;
    }

    filter->patterns[filter->pattern_count] = strdup(pattern);
    if (!filter->patterns[filter->pattern_count]) {
        csilk_mutex_unlock(&filter->mutex);
        return -1;
    }

    filter->pattern_count++;
    csilk_mutex_unlock(&filter->mutex);
    return 0;
}

csilk_waf_action_t
csilk_waf_filter_inspect(csilk_waf_filter_t* filter,
                         uint32_t            src_ip,
                         const uint8_t*      payload,
                         size_t              len)
{
    if (!filter) {
        return CSILK_WAF_ACTION_PASS;
    }

    csilk_mutex_lock(&filter->mutex);

    for (size_t i = 0; i < filter->rule_count; i++) {
        uint32_t plen = filter->rules[i].prefix_len;
        uint32_t mask = (plen == 0) ? 0U : (plen >= 32U ? 0xFFFFFFFFU : (~0U << (32U - plen)));
        if ((src_ip & mask) == (filter->rules[i].ip & mask) &&
            filter->rules[i].action == CSILK_WAF_ACTION_DROP) {
            csilk_mutex_unlock(&filter->mutex);
            return CSILK_WAF_ACTION_DROP;
        }
    }

    if (payload && len > 0) {
        for (size_t i = 0; i < filter->pattern_count; i++) {
            if (filter->patterns[i] &&
                _csilk_waf_memmem(payload, len, filter->patterns[i], strlen(filter->patterns[i]))) {
                csilk_mutex_unlock(&filter->mutex);
                return CSILK_WAF_ACTION_DROP;
            }
        }
    }

    csilk_mutex_unlock(&filter->mutex);
    return CSILK_WAF_ACTION_PASS;
}

void
csilk_waf_filter_free(csilk_waf_filter_t* filter)
{
    if (!filter) {
        return;
    }
    csilk_mutex_lock(&filter->mutex);
    for (size_t i = 0; i < filter->pattern_count; i++) {
        free(filter->patterns[i]);
    }
    csilk_mutex_unlock(&filter->mutex);
    csilk_mutex_destroy(&filter->mutex);
    free(filter);
}
