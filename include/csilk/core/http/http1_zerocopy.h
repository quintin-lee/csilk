#pragma once
/**
 * @file http1_zerocopy.h
 * @brief Zero-copy HTTP/1.1 header slicing and string-view helpers.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include "csilk/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zero-copy HTTP/1.1 header name/value string-view pair.
 */
typedef struct {
    csilk_str_view_t name;  /**< Borrowed slice referencing the header name. */
    csilk_str_view_t value; /**< Borrowed slice referencing the header value. */
} csilk_header_slice_t;

/**
 * @brief Compare a string view against a NUL-terminated string for equality.
 *
 * @param[in] view String view to compare.
 * @param[in] str  NUL-terminated C string to compare against.
 * @return 1 if the view equals @p str (same length and content), 0 otherwise.
 */
int csilk_str_view_equal(csilk_str_view_t view, const char* str);

/**
 * @brief Slice the HTTP/1 header block of a request buffer into name/value views.
 *
 * @param[in]  buf             Flat request buffer (must contain a request line).
 * @param[in]  len             Length in bytes of @p buf.
 * @param[out] slices          Caller-allocated array to receive name/value views.
 * @param[in]  max_slices      Capacity of the @p slices array.
 * @param[out] out_slice_count Number of header slices written (may be NULL).
 * @return 0 on success, -1 on invalid input or missing request line.
 */
int csilk_http1_parse_header_slices(const char*           buf,
                                    size_t                len,
                                    csilk_header_slice_t* slices,
                                    size_t                max_slices,
                                    size_t*               out_slice_count);

#ifdef __cplusplus
}
#endif
