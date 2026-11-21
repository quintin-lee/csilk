/**
 * @file multipart.c
 * @brief Multipart/form-data request body parsing implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"

/** @brief Maximum multipart form part headers. */
enum { CSILK_MAX_PART_HEADERS = 32 };
/** @brief Maximum multipart form part name length. */
enum { CSILK_MAX_PART_NAME = 128 };
/** @brief Maximum multipart form part filename length. */
enum { CSILK_MAX_PART_FILENAME = 256 };
/** @brief Maximum multipart form part body size (10 MB). */
enum { CSILK_MAX_PART_SIZE = 10 * 1024 * 1024 };

/**
 * @brief Parse a multipart/form-data request body and invoke a handler for
 *        each part.
 *
 * Extracts the boundary string from the Content-Type header, then iterates
 * through the request body looking for boundary-delimited parts. For each
 * part, the function parses the Content-Disposition header (extracting name
 * and optional filename), the optional Content-Type header, and the part
 * body. A csilk_multipart_part_t struct is populated and passed to the
 * caller-supplied handler callback.
 *
 * @param c       The request context containing the parsed body.
 * @param handler Callback invoked once per parsed part. Receives a pointer
 *                to a csilk_multipart_part_t describing the part. Must not
 *                be NULL.
 *
 * @note The part data pointers reference memory within the request body
 *       buffer directly — they are NOT copies. The handler must not modify
 *       or store the pointers beyond its invocation.
 * @warning Part body data is NOT null-terminated. Use part.data_len to
 *          determine the bounds.
 * @warning Maximum part name length is 127 bytes, maximum filename length
 *          is 255 bytes, and maximum part headers per section is 32. Parts
 *          exceeding these limits will be silently truncated.
 */
/** @brief Binary-safe substring search within bounded memory. */
static const void*
_csilk_memmem(const void* haystack, size_t haystack_len, const void* needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    const uint8_t* h = (const uint8_t*)haystack;
    const uint8_t* n = (const uint8_t*)needle;
    const uint8_t* end = h + (haystack_len - needle_len);
    for (const uint8_t* p = h; p <= end; p++) {
        if (*p == *n && memcmp(p, n, needle_len) == 0) {
            return (const void*)p;
        }
    }
    return NULL;
}

void
csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler)
{
    if (!c || !handler) {
        CSILK_LOG_W("Multipart: parser skipped - context or handler is missing");
        return;
    }

    size_t      data_len = 0;
    const char* data = csilk_get_body(c, &data_len);
    if (!data || data_len == 0) {
        CSILK_LOG_W("Multipart: parser skipped - body is missing or empty");
        return;
    }

    CSILK_LOG_T("Multipart: starting parser for request %p", (void*)c);

    const char* content_type = csilk_get_header(c, "Content-Type");
    if (!content_type) {
        CSILK_LOG_W("Multipart: parser skipped - missing Content-Type header");
        return;
    }

    const char* boundary_prefix = "multipart/form-data; boundary=";
    if (strncmp(content_type, boundary_prefix, strlen(boundary_prefix)) != 0) {
        CSILK_LOG_W("Multipart: parser skipped - Content-Type '%s' is not multipart/form-data",
                    content_type);
        return;
    }

    const char* boundary = content_type + strlen(boundary_prefix);
    if (!boundary || strlen(boundary) == 0) {
        CSILK_LOG_W("Multipart: parser skipped - empty boundary string in Content-Type");
        return;
    }

    char delimiter[256];
    snprintf(delimiter, sizeof(delimiter), "--%s", boundary);
    size_t delim_len = strlen(delimiter);

    size_t end_delim_len = delim_len + 2; // --boundary--

    if (!data || data_len == 0) {
        return;
    }

    const char* pos = data;
    const char* end = data + data_len;

    /* Expect the first boundary marker at the very start of the body.
     * RFC 2046 §5.1.1: the body must begin with "--boundary\r\n". */
    if ((size_t)(end - pos) < delim_len || memcmp(pos, delimiter, delim_len) != 0) {
        CSILK_LOG_E("Multipart: invalid payload structure - first boundary marker not "
                    "found at start of body");
        return;
    }
    pos += delim_len;
    if (pos < end && *pos == '\r') {
        pos++;
    }
    if (pos < end && *pos == '\n') {
        pos++;
    }

    /* Iterate over each part in the multipart body.
     * Each part consists of headers (terminated by \r\n) followed by
     * the part body, then the next boundary delimiter. */
    while (pos < end) {
        csilk_multipart_part_t part;
        memset(&part, 0, sizeof(part));

        CSILK_LOG_T("Multipart: parsing part at byte offset %td", pos - data);

        /* Parse part headers strictly within [pos, end). */
        while (pos < end && *pos != '\r' && *pos != '\n') {
            const char* line_end = (const char*)_csilk_memmem(pos, (size_t)(end - pos), "\r\n", 2);
            const char* hdr_end = line_end ? line_end : end;
            size_t      line_len = (size_t)(hdr_end - pos);

            if (line_len >= 20 && memcmp(pos, "Content-Disposition:", 20) == 0) {
                const char* cur = pos + 20;
                while (cur < hdr_end && *cur == ' ') {
                    cur++;
                }

                const char* name_start =
                    (const char*)_csilk_memmem(cur, (size_t)(hdr_end - cur), "name=\"", 6);
                if (name_start) {
                    name_start += 6;
                    const char* name_end =
                        (const char*)memchr(name_start, '"', (size_t)(hdr_end - name_start));
                    if (name_end) {
                        size_t nlen = (size_t)(name_end - name_start);
                        if (nlen < CSILK_MAX_PART_NAME) {
                            memcpy(part.name, name_start, nlen);
                            part.name[nlen] = '\0';
                        } else {
                            CSILK_LOG_W("Multipart: part name truncated - "
                                        "exceeds limit of %d bytes",
                                        CSILK_MAX_PART_NAME);
                        }
                    }
                }

                const char* filename_start =
                    (const char*)_csilk_memmem(cur, (size_t)(hdr_end - cur), "filename=\"", 10);
                if (filename_start) {
                    filename_start += 10;
                    const char* filename_end = (const char*)memchr(
                        filename_start, '"', (size_t)(hdr_end - filename_start));
                    if (filename_end) {
                        size_t flen = (size_t)(filename_end - filename_start);
                        if (flen < CSILK_MAX_PART_FILENAME) {
                            memcpy(part.filename, filename_start, flen);
                            part.filename[flen] = '\0';
                        } else {
                            CSILK_LOG_W("Multipart: filename truncated "
                                        "- exceeds limit of %d bytes",
                                        CSILK_MAX_PART_FILENAME);
                        }
                    }
                }
            } else if (line_len >= 13 && memcmp(pos, "Content-Type:", 13) == 0) {
                const char* cur = pos + 13;
                while (cur < hdr_end && *cur == ' ') {
                    cur++;
                }
                size_t ct_len = (size_t)(hdr_end - cur);
                if (ct_len < sizeof(part.content_type)) {
                    memcpy(part.content_type, cur, ct_len);
                    part.content_type[ct_len] = '\0';
                }
            }

            if (line_end) {
                pos = line_end + 2;
            } else {
                pos = end;
                break;
            }
        }

        /* Blank line marks the end of headers (RFC 5322 §2.1). */
        if ((size_t)(end - pos) >= 2 && pos[0] == '\r' && pos[1] == '\n') {
            pos += 2;
        } else if (pos < end && *pos == '\n') {
            pos += 1;
        }

        /* Locate the part body: scan forward for the next boundary delimiter. */
        const char* body_start = pos;
        const char* body_end = NULL;
        const char* search = pos;
        while (search < end) {
            size_t      remaining = (size_t)(end - search);
            const char* next_boundary =
                (const char*)_csilk_memmem(search, remaining, delimiter, delim_len);
            if (!next_boundary) {
                body_end = end;
                break;
            }
            if (next_boundary > pos && *(next_boundary - 1) == '\n') {
                body_end = next_boundary - 1;
                if (body_end > body_start && *(body_end - 1) == '\r') {
                    body_end--;
                }
                break;
            }
            search = next_boundary + delim_len;
        }
        if (!body_end) {
            body_end = end;
        }

        if (body_end > body_start) {
            part.data = (uint8_t*)body_start;
            part.data_len = (size_t)(body_end - body_start);
        }

        CSILK_LOG_D("Multipart: parsed part - name: '%s', filename: '%s', content-type: "
                    "'%s', size: %zu bytes",
                    part.name,
                    part.filename,
                    part.content_type,
                    part.data_len);

        part.ctx = c;

        /* Check part size limit to prevent DoS (CWE-434) */
        if (part.data_len > CSILK_MAX_PART_SIZE) {
            CSILK_LOG_E("Multipart: Part '%s' exceeds size limit (%zu > %d bytes)",
                        part.name[0] ? part.name : "(unnamed)",
                        part.data_len,
                        CSILK_MAX_PART_SIZE);
            csilk_status(c, 413); /* Payload Too Large */
            return;
        }

        CSILK_LOG_T("Multipart: invoking user handler for part '%s'", part.name);
        handler(&part);

        pos = body_end;
        if ((size_t)(end - pos) >= 2 && pos[0] == '\r' && pos[1] == '\n') {
            pos += 2;
        } else if (pos < end && *pos == '\n') {
            pos += 1;
        }
        if ((size_t)(end - pos) < delim_len) {
            CSILK_LOG_D("Multipart: reached end of payload or remaining data too short "
                        "for boundary");
            break;
        }
        if (memcmp(pos, delimiter, delim_len) != 0) {
            CSILK_LOG_W("Multipart: parsing break - expected boundary delimiter");
            break;
        }
        if ((size_t)(end - pos) >= end_delim_len && memcmp(pos + delim_len, "--", 2) == 0) {
            CSILK_LOG_D("Multipart: reached final boundary marker '--%s--'", boundary);
            break;
        }
        pos += delim_len;
        if (pos < end && *pos == '\r') {
            pos++;
        }
        if (pos < end && *pos == '\n') {
            pos++;
        }
    }

    CSILK_LOG_T("Multipart: parser execution finished for request %p", (void*)c);
}
