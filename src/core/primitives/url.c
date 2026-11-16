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

#include "core/ctx/ctx_internal.h"
#include "csilk/core/internal.h"

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
 * space. Invalid percent-encodings are normalised by escaping the '%' as
 * '%25' (i.e. "%%ZZ" becomes "%25ZZ"), so that WAF regexes match on the
 * canonical form (CWE-116 / RFC 3986 §2.1).
 *
 * The decode is done in-place: the output is written starting at index 0
 * of @p str.  Because escaping an invalid '%' expands one input byte into
 * three output bytes, the decoded output can reach up to 3 * strlen(str)
 * bytes.  All in-tree callers (csilk_split_url, csilk_parse_query,
 * http1_parse.c) therefore allocate a buffer sized to that 3x worst case
 * (3 * input_len + 1) so the in-place decode never overflows.
 *
 * @param str Null-terminated string to decode (modified in-place).
 * @return The length of the decoded string.
 */
size_t
csilk_url_decode(char* str, size_t str_cap)
{
    if (!str || str_cap == 0) {
        return 0;
    }

    /* Two-pass in-place decode.
     *
     * Pass 1: measure the output length.  If the output fits within the
     * input length, a forward in-place decode is safe (dst ≤ src always).
     * If the output is longer, we use a malloc'd temporary buffer to avoid
     * overwriting unread source. */
    size_t      out_len = 0;
    const char* p = str;
    while (*p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int hi = hex_to_int(p[1]);
            int lo = hex_to_int(p[2]);
            if (hi >= 0 && lo >= 0) {
                out_len++;
                p += 3;
                continue;
            }
        }
        if (*p == '%') {
            out_len += 3; /* invalid '%' → "%25" (3 bytes) */
            p++;
            continue;
        }
        out_len++;
        p++;
    }
    size_t in_len = p - str;

    /* Use a heap buffer whenever the input contains '%' characters.
     * This avoids the in-place overwrite problem entirely.  The overhead
     * is one malloc+free per call, which is acceptable for URL decoding. */
    int has_percent = (int)(memchr(str, '%', in_len) != NULL);

    if (!has_percent) {
        /* No '%' in input: pure in-place, no expansion possible. */
        char* src = str;
        char* dst = str;
        while (*src) {
            if (*src == '+') {
                *dst++ = ' ';
            } else {
                *dst++ = *src;
            }
            src++;
        }
        *dst = '\0';
        return out_len;
    }

    /* Input contains '%': use a heap buffer to safely handle both valid
     * %XX (1 byte) and invalid % (3 bytes "%25") expansions.  The buffer is
     * sized to the absolute worst case, 3 * in_len + 1 (every input byte can
     * expand to 3 bytes), which is provably >= the decoded output length.
     * Sizing from in_len (rather than the measured out_len) keeps the
     * capacity relationship transparent to static analysis. */
    size_t tmp_cap = in_len * 3 + 1;
    char*  tmp = malloc(tmp_cap);
    if (!tmp) {
        /* Fallback: leave the string unchanged (safe, no crash). */
        return in_len;
    }
    char* src = str;
    char* dst = tmp;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex_to_int(src[1]);
            int lo = hex_to_int(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
        } else if (*src == '%') {
            *dst++ = '%';
            *dst++ = '2';
            *dst++ = '5';
            src++;
            continue;
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
    /* The decoded result is out_len bytes + NUL (out_len <= in_len * 3).
     * Write as much as fits in str (bounded by str_cap); if the output does
     * not fit, truncate but always NUL-terminate.  The copy length is
     * min(out_len + 1, str_cap). */
    size_t copy_len = (out_len + 1 < str_cap) ? (out_len + 1) : str_cap;
    if (copy_len > 0) {
        memcpy(str, tmp, copy_len);
        /* When truncated, ensure the string is NUL-terminated at str_cap-1.
         * When not truncated, the NUL was already copied as the last byte. */
        if (copy_len <= out_len) {
            str[copy_len - 1] = '\0';
        }
    }
    free(tmp);
    return out_len;
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
        /* Worst case: every char is an invalid '%' that expands to "%25",
         * i.e. output length can reach 3 * path_len. Allocate to that bound
         * plus the NUL terminator so the in-place decode never overflows. */
        *path = malloc(path_len * 3 + 1);
        if (!*path) {
            return;
        }

        memcpy(*path, url, path_len);
        (*path)[path_len] = '\0';
        csilk_url_decode(*path, path_len * 3 + 1);

        *query = strdup(qmark + 1);
        if (!*query) {
            free(*path);
            *path = NULL;
        }
    } else {
        size_t url_len = strlen(url);
        /* Same 3x worst-case bound for the decoded path. */
        *path = malloc(url_len * 3 + 1);
        if (*path) {
            memcpy(*path, url, url_len + 1);
            csilk_url_decode(*path, url_len * 3 + 1);
        }
    }
}
