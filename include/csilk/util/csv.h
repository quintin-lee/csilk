#pragma once
/**
 * @file csilk/util/csv.h
 * @brief RFC 4180 compliant CSV string escaping and streaming parsing engine.
 * @copyright MIT License
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Escape a string value for RFC 4180 CSV serialization.
 *
 * If the string contains comma (,), double-quote ("), newline (\n), or CR (\r),
 * it is wrapped in double quotes and inner quotes are doubled ("").
 *
 * @param[out] out     Output buffer.
 * @param[in]  out_sz  Output buffer capacity.
 * @param[in]  in      Input string (if NULL, outputs empty string).
 * @return Number of characters written (or would be written).
 */
int csilk_csv_escape(char* out, size_t out_sz, const char* in);

/**
 * @brief Parse a single CSV field from a line buffer.
 *
 * Handles quoted fields with escaped quotes ("") and unquoted fields.
 *
 * @param[in]  line           Input line pointer.
 * @param[in]  len            Remaining length of input line.
 * @param[out] out            Output buffer for decoded field.
 * @param[in]  out_sz         Capacity of output buffer.
 * @param[out] chars_consumed Total characters consumed from line (including delimiters).
 * @return 0 on success, -1 on failure.
 */
int csilk_csv_parse_field(
    const char* line, size_t len, char* out, size_t out_sz, size_t* chars_consumed);

/**
 * @brief Parse a full CSV row into a fixed 12x512 array.
 *
 * @param[in]  line  Input line pointer.
 * @param[in]  len   Length of line (including optional CRLF).
 * @param[out] out   12 x 512 array of field buffers.
 * @param[out] count Pointer to receive number of parsed fields.
 * @return Number of fields parsed (*count).
 */
int csilk_csv_parse_row(const char* line, size_t len, char out[12][512], int* count);

/**
 * @brief Parse a full CSV row into dynamic array pointers.
 *
 * @param[in]  line       Input line pointer.
 * @param[in]  len        Length of line.
 * @param[out] out_fields Array of field buffers.
 * @param[in]  field_cap  Capacity of each field buffer.
 * @param[in]  max_fields Maximum number of fields in out_fields.
 * @param[out] count      Pointer to receive number of parsed fields.
 * @return Number of fields parsed.
 */
int csilk_csv_parse_row_ext(const char* line,
                            size_t      len,
                            char*       out_fields[],
                            size_t      field_cap,
                            size_t      max_fields,
                            int*        count);

#ifdef __cplusplus
}
#endif
