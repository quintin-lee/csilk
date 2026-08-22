#pragma once
/**
 * @file swar_http.h
 * @brief SWAR (SIMD Within A Register) branchless HTTP/1.1 header scanner.
 *
 * @version 0.5.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include "csilk/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fast SWAR scan to find the first occurrence of a character in a buffer.
 *
 * Operates on 64-bit words (8 bytes at a time) using bitwise parallel arithmetic.
 *
 * @param buf Pointer to buffer.
 * @param len Buffer length in bytes.
 * @param target Character byte to find.
 * @return Index of the first matching byte, or (size_t)-1 if not found.
 */
size_t csilk_swar_find_char(const char* buf, size_t len, char target);

/**
 * @brief Fast SWAR scan to find header delimiter CRLF ("\r\n").
 *
 * @param buf Pointer to buffer.
 * @param len Buffer length in bytes.
 * @return Offset to the start of "\r\n", or (size_t)-1 if not found.
 */
size_t csilk_swar_find_crlf(const char* buf, size_t len);

/**
 * @brief Branchless HTTP header line parser using SWAR index scanning.
 *
 * Splits a line like "Host: example.com" into field and value views.
 *
 * @param line Pointer to line text.
 * @param len Line length in bytes.
 * @param[out] field Receives field name string view.
 * @param[out] value Receives field value string view.
 * @return 0 on success, -1 if invalid header line.
 */
int csilk_swar_parse_header_line(const char*       line,
                                 size_t            len,
                                 csilk_str_view_t* field,
                                 csilk_str_view_t* value);

#ifdef __cplusplus
}
#endif
