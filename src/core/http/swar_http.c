/**
 * @file swar_http.c
 * @brief SWAR (SIMD Within A Register) branchless HTTP/1.1 header scanner implementation.
 * @copyright MIT License
 */

#include "csilk/core/swar_http.h"
#include <string.h>

static inline uint64_t
has_zero_byte(uint64_t v)
{
    return (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;
}

static inline uint64_t
match_byte(uint64_t v, uint8_t c)
{
    uint64_t target_word = 0x0101010101010101ULL * c;
    return has_zero_byte(v ^ target_word);
}

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
