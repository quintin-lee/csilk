/**
 * @file src/util/csv.c
 * @brief RFC 4180 compliant CSV escaping and streaming parsing implementation.
 * @copyright MIT License
 */

#include "csilk/util/csv.h"

#include <stdio.h>
#include <string.h>

int
csilk_csv_escape(char* out, size_t out_sz, const char* in)
{
    if (!out || out_sz == 0) {
        return -1;
    }
    if (!in) {
        *out = '\0';
        return 0;
    }

    int needs_quote = 0;
    for (const char* p = in; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        return snprintf(out, out_sz, "%s", in);
    }

    size_t j = 0;
    out[j++] = '"';
    for (const char* p = in; *p && j < out_sz - 2; p++) {
        if (*p == '"') {
            out[j++] = '"';
            if (j < out_sz - 2) {
                out[j++] = '"';
            }
        } else {
            out[j++] = *p;
        }
    }
    if (j < out_sz - 1) {
        out[j++] = '"';
    }
    out[j] = '\0';
    return (int)j;
}

int
csilk_csv_parse_field(
    const char* line, size_t len, char* out, size_t out_sz, size_t* chars_consumed)
{
    if (!line || !out || out_sz == 0) {
        return -1;
    }

    size_t pos = 0;
    if (pos >= len || line[pos] != '"') {
        while (pos < len && line[pos] != ',' && line[pos] != '\n' && line[pos] != '\r') {
            pos++;
        }
        size_t n = pos < out_sz - 1 ? pos : out_sz - 1;
        memcpy(out, line, n);
        out[n] = '\0';
        if (chars_consumed) {
            *chars_consumed = pos;
        }
        return 0;
    }

    pos++;
    size_t oi = 0;
    while (pos < len) {
        if (line[pos] == '"' && pos + 1 < len && line[pos + 1] == '"') {
            if (oi < out_sz - 1) {
                out[oi++] = '"';
            }
            pos += 2;
        } else if (line[pos] == '"') {
            pos++;
            if (chars_consumed) {
                *chars_consumed = pos;
            }
            out[oi] = '\0';
            return 0;
        } else {
            if (oi < out_sz - 1) {
                out[oi++] = line[pos];
            }
            pos++;
        }
    }
    out[oi] = '\0';
    if (chars_consumed) {
        *chars_consumed = pos;
    }
    return 0;
}

int
csilk_csv_parse_row_ext(const char* line,
                        size_t      len,
                        char*       out_fields[],
                        size_t      field_cap,
                        size_t      max_fields,
                        int*        count)
{
    if (!line || !out_fields || !count || max_fields == 0) {
        return -1;
    }

    size_t pos = 0;
    *count = 0;
    while (pos < len) {
        size_t consumed = 0;
        if ((size_t)*count >= max_fields) {
            break;
        }
        csilk_csv_parse_field(line + pos, len - pos, out_fields[*count], field_cap, &consumed);
        pos += consumed;
        (*count)++;
        if (pos >= len || line[pos] == '\n' || line[pos] == '\r') {
            if (pos < len && line[pos] == '\r') {
                pos++;
            }
            if (pos < len && line[pos] == '\n') {
                pos++;
            }
            break;
        }
        if (pos < len && line[pos] == ',') {
            pos++;
        }
    }
    return *count;
}

int
csilk_csv_parse_row(const char* line, size_t len, char out[12][512], int* count)
{
    if (!line || !out || !count) {
        return -1;
    }
    char* fields[12];
    for (int i = 0; i < 12; i++) {
        fields[i] = out[i];
    }
    return csilk_csv_parse_row_ext(line, len, fields, 512, 12, count);
}
