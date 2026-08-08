#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/io/af_xdp_internal.h"

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
