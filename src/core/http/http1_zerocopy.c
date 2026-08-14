/**
 * @file http1_zerocopy.c
 * @brief Zero-copy HTTP/1 parsing helpers backed by string views.
 *
 * Provides lightweight, allocation-free routines for comparing string views
 * against NUL-terminated strings and for slicing HTTP/1 header lines out of a
 * flat request buffer without copying the underlying data.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "csilk/csilk.h"
#include "core/ctx/ctx_internal.h"
#include "core/io/af_xdp_internal.h"

int
csilk_view_cmp(csilk_view_t view, const char* str)
{
    if (!str && !view.ptr) {
        return 0;
    }
    if (!view.ptr) {
        return -1;
    }
    if (!str) {
        return 1;
    }
    size_t str_len = strlen(str);
    size_t min_len = view.len < str_len ? view.len : str_len;
    int    r = memcmp(view.ptr, str, min_len);
    if (r != 0) {
        return r;
    }
    if (view.len < str_len) {
        return -1;
    }
    if (view.len > str_len) {
        return 1;
    }
    return 0;
}

int
csilk_view_casecmp(csilk_view_t view, const char* str)
{
    if (!str && !view.ptr) {
        return 0;
    }
    if (!view.ptr) {
        return -1;
    }
    if (!str) {
        return 1;
    }
    size_t str_len = strlen(str);
    size_t min_len = view.len < str_len ? view.len : str_len;
    int    r = strncasecmp(view.ptr, str, min_len);
    if (r != 0) {
        return r;
    }
    if (view.len < str_len) {
        return -1;
    }
    if (view.len > str_len) {
        return 1;
    }
    return 0;
}

int
csilk_view_equal(csilk_view_t a, csilk_view_t b)
{
    if (a.len != b.len) {
        return 0;
    }
    if (a.len == 0) {
        return 1;
    }
    if (!a.ptr || !b.ptr) {
        return a.ptr == b.ptr;
    }
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

const char*
csilk_view_to_arena(csilk_ctx_t* c, csilk_view_t view)
{
    if (!c || !c->arena || !view.ptr) {
        return NULL;
    }
    return csilk_arena_strndup(c->arena, view.ptr, view.len);
}

char*
csilk_view_to_heap(csilk_view_t view)
{
    if (!view.ptr) {
        return NULL;
    }
    char* copy = malloc(view.len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, view.ptr, view.len);
    copy[view.len] = '\0';
    return copy;
}

char*
csilk_str_view_to_string(const csilk_str_view_t* view)
{
    if (!view) {
        return NULL;
    }
    return csilk_view_to_heap(*view);
}

const char*
csilk_str_view_persist(csilk_ctx_t* c, const csilk_str_view_t* view)
{
    if (!view) {
        return NULL;
    }
    return csilk_view_to_arena(c, *view);
}

/**
 * @brief Compare a string view against a NUL-terminated string for equality.
 * @param[in] view String view to compare.
 * @param[in] str NUL-terminated C string to compare against.
 * @return 1 if the view equals str (same length and content), 0 otherwise.
 * @note Returns 0 if either argument is NULL or the view has a NULL data
 *       pointer. Comparison is bounded by view.len and is not NUL-sensitive.
 */
int
csilk_str_view_equal(csilk_str_view_t view, const char* str)
{
    if (!str || !view.data) {
        return 0;
    }
    size_t str_len = strlen(str);
    if (view.len != str_len) {
        return 0;
    }
    return strncmp(view.data, str, view.len) == 0;
}

/**
 * @brief Slice the HTTP/1 header block of a request buffer into name/value views.
 * @param[in]  buf         Flat request buffer (must contain a request line).
 * @param[in]  len         Length in bytes of buf.
 * @param[out] slices      Caller-allocated array to receive name/value views.
 * @param[in]  max_slices  Capacity of the slices array.
 * @param[out] out_slice_count Number of header slices written (may be NULL).
 * @return 0 on success, -1 on invalid input or missing request line.
 * @note The views point directly into buf; the buffer must outlive the slices.
 *       Parsing stops at the first empty line, a missing ':' on a line, or when
 *       max_slices is reached. Leading OWS (space/tab) is stripped from values.
 */
int
csilk_http1_parse_header_slices(const char*           buf,
                                size_t                len,
                                csilk_header_slice_t* slices,
                                size_t                max_slices,
                                size_t*               out_slice_count)
{
    if (!buf || len == 0 || !slices || max_slices == 0) {
        return -1;
    }

    size_t      count = 0;
    const char* ptr = buf;
    const char* end = buf + len;

    /* Skip request line */
    const char* line_end = memchr(ptr, '\n', (size_t)(end - ptr));
    if (!line_end) {
        return -1;
    }
    ptr = line_end + 1;

    while (ptr < end && count < max_slices) {
        if (*ptr == '\r' || *ptr == '\n') {
            break; /* End of headers */
        }

        const char* colon = memchr(ptr, ':', (size_t)(end - ptr));
        if (!colon) {
            break;
        }

        const char* next_line = memchr(colon, '\n', (size_t)(end - colon));
        if (!next_line) {
            break;
        }

        slices[count].name.data = ptr;
        slices[count].name.len = (size_t)(colon - ptr);

        const char* val_start = colon + 1;
        while (val_start < next_line && (*val_start == ' ' || *val_start == '\t')) {
            val_start++;
        }

        const char* val_end = next_line;
        if (val_end > val_start && *(val_end - 1) == '\r') {
            val_end--;
        }

        slices[count].value.data = val_start;
        slices[count].value.len = (size_t)(val_end - val_start);
        count++;

        ptr = next_line + 1;
    }

    if (out_slice_count) {
        *out_slice_count = count;
    }
    return 0;
}
