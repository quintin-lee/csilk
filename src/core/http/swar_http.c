/**
 * @file swar_http.c
 * @brief SWAR (SIMD Within A Register) branchless HTTP/1.1 header scanner implementation.
 * @copyright MIT License
 */

#include "csilk/core/swar_http.h"
#include <string.h>

/** @brief SWAR test for any zero byte within a 64-bit word. */
static inline uint64_t
has_zero_byte(uint64_t v)
{
    return (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;
}

/** @brief SWAR test returning a mask for bytes in v equal to c. */
static inline uint64_t
match_byte(uint64_t v, uint8_t c)
{
    uint64_t target_word = 0x0101010101010101ULL * c;
    return has_zero_byte(v ^ target_word);
}

/**
 * @brief Find the first occurrence of target in a buffer using SWAR scanning.
 * @param[in] buf    Buffer to search (may be non-NUL-terminated).
 * @param[in] len    Length of buf in bytes.
 * @param[in] target Byte to locate.
 * @return Index of the first match, or (size_t)-1 if not found or on NULL/empty.
 * @note Scans 8-byte words in parallel (with a scalar tail) to stay branch-light.
 */
size_t
csilk_swar_find_char(const char* buf, size_t len, char target)
{
    if (!buf || len == 0) {
        return (size_t)-1;
    }

    size_t  i = 0;
    uint8_t c = (uint8_t)target;

    /* Process 8-byte blocks in parallel via SWAR */
    while (i + 8 <= len) {
        uint64_t word;
        memcpy(&word, buf + i, sizeof(word));
        uint64_t match = match_byte(word, c);
        if (match != 0) {
#if defined(__GNUC__) || defined(__clang__)
            size_t byte_idx = __builtin_ctzll(match) >> 3;
            return i + byte_idx;
#else
            for (size_t b = 0; b < 8; b++) {
                if (buf[i + b] == target) {
                    return i + b;
                }
            }
#endif
        }
        i += 8;
    }

    /* Process remaining tail bytes */
    while (i < len) {
        if (buf[i] == target) {
            return i;
        }
        i++;
    }

    return (size_t)-1;
}

/**
 * @brief Find the position of a CRLF ("\r\n") sequence in a buffer using SWAR.
 * @param[in] buf Buffer to search.
 * @param[in] len Length of buf in bytes (must be >= 2).
 * @return Index of the '\r' of the first CRLF, or (size_t)-1 if none is found.
 * @note Delegates to csilk_swar_find_char for the '\r' and verifies the
 *       following byte is '\n'; advances past each candidate to skip false hits.
 */
size_t
csilk_swar_find_crlf(const char* buf, size_t len)
{
    if (!buf || len < 2) {
        return (size_t)-1;
    }

    size_t pos = 0;
    while (pos < len - 1) {
        size_t idx = csilk_swar_find_char(buf + pos, len - pos, '\r');
        if (idx == (size_t)-1 || pos + idx + 1 >= len) {
            break;
        }
        if (buf[pos + idx + 1] == '\n') {
            return pos + idx;
        }
        pos += idx + 1;
    }

    return (size_t)-1;
}

/**
 * @brief Parse a single HTTP header line into field/value string views (SWAR).
 * @param[in]  line  Header line buffer (not required to be NUL-terminated).
 * @param[in]  len   Length of line in bytes.
 * @param[out] field Receives the header name view (excludes the ':').
 * @param[out] value Receives the header value view (OWS stripped, no trailing CR/LF).
 * @return 0 on success, -1 on NULL args, empty line, or a missing/leading ':'.
 * @note Views point into line; callers must keep the buffer alive. Uses SWAR
 *       (SIMD-within-a-register) scans for the colon and trailing whitespace.
 */
int
csilk_swar_parse_header_line(const char*       line,
                             size_t            len,
                             csilk_str_view_t* field,
                             csilk_str_view_t* value)
{
    if (!line || len == 0 || !field || !value) {
        return -1;
    }

    size_t colon_pos = csilk_swar_find_char(line, len, ':');
    if (colon_pos == (size_t)-1 || colon_pos == 0) {
        return -1;
    }

    field->data = line;
    field->len = colon_pos;

    size_t val_start = colon_pos + 1;
    while (val_start < len && (line[val_start] == ' ' || line[val_start] == '\t')) {
        val_start++;
    }

    size_t val_end = len;
    while (val_end > val_start && (line[val_end - 1] == '\r' || line[val_end - 1] == '\n' ||
                                   line[val_end - 1] == ' ' || line[val_end - 1] == '\t')) {
        val_end--;
    }

    value->data = line + val_start;
    value->len = val_end - val_start;

    return 0;
}
