/**
 * @file multipart.c
 * @brief Multipart/form-data request body parsing implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk.h"

#define CSILK_MAX_PART_HEADERS 32
#define CSILK_MAX_PART_NAME 128
#define CSILK_MAX_PART_FILENAME 256

/** @brief Parse multipart/form-data request body and invoke handler for each part. */
void csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler) {
    if (!c || !c->request.body || !handler) return;

    const char* content_type = csilk_get_header(c, "Content-Type");
    if (!content_type) return;

    const char* boundary_prefix = "multipart/form-data; boundary=";
    if (strncmp(content_type, boundary_prefix, strlen(boundary_prefix)) != 0) return;

    const char* boundary = content_type + strlen(boundary_prefix);
    if (!boundary || strlen(boundary) == 0) return;

    char delimiter[256];
    snprintf(delimiter, sizeof(delimiter), "--%s", boundary);
    size_t delim_len = strlen(delimiter);

    size_t end_delim_len = delim_len + 2; // --boundary--

    const char* data = c->request.body;
    size_t data_len = c->request.body_len;

    const char* pos = data;

    if (strncmp(pos, delimiter, delim_len) != 0) return;
    pos += delim_len;
    if (*pos == '\r') pos++;
    if (*pos == '\n') pos++;

    while (pos < data + data_len) {
        if (pos + delim_len > data + data_len) break;
        if (strncmp(pos, delimiter, delim_len) != 0) break;

        pos += delim_len;
        if (pos < data + data_len && *pos == '\r') pos++;
        if (pos < data + data_len && *pos == '\n') pos++;

        csilk_multipart_part_t part;
        memset(&part, 0, sizeof(part));

        while (pos < data + data_len && *pos != '\r') {
            if (strncmp(pos, "Content-Disposition:", 20) == 0) {
                pos += 20;
                while (pos < data + data_len && *pos == ' ') pos++;

                const char* name_start = strstr(pos, "name=\"");
                if (name_start) {
                    name_start += 6;
                    const char* name_end = strchr(name_start, '"');
                    if (name_end) {
                        size_t nlen = name_end - name_start;
                        if (nlen < CSILK_MAX_PART_NAME) {
                            memcpy(part.name, name_start, nlen);
                            part.name[nlen] = '\0';
                        }
                    }
                }

                const char* filename_start = strstr(pos, "filename=\"");
                if (filename_start) {
                    filename_start += 10;
                    const char* filename_end = strchr(filename_start, '"');
                    if (filename_end) {
                        size_t flen = filename_end - filename_start;
                        if (flen < CSILK_MAX_PART_FILENAME) {
                            memcpy(part.filename, filename_start, flen);
                            part.filename[flen] = '\0';
                        }
                    }
                }
            } else if (strncmp(pos, "Content-Type:", 13) == 0) {
                pos += 13;
                while (pos < data + data_len && *pos == ' ') pos++;
                const char* ct_end = strstr(pos, "\r\n");
                size_t ct_len = ct_end ? (size_t)(ct_end - pos) : strlen(pos);
                if (ct_len < sizeof(part.content_type) - 1) {
                    memcpy(part.content_type, pos, ct_len);
                    part.content_type[ct_len] = '\0';
                }
                if (ct_end) pos = ct_end;
                else pos += ct_len;
                continue;
            }

            const char* line_end = strstr(pos, "\r\n");
            if (line_end) pos = line_end + 2;
            else break;
        }

        if (pos < data + data_len && strncmp(pos, "\r\n", 2) == 0) pos += 2;

        const char* body_start = pos;

        const char* body_end = NULL;
        const char* search = pos;
        while (search < data + data_len) {
            const char* next_boundary = strstr(search, delimiter);
            if (!next_boundary) {
                body_end = data + data_len;
                break;
            }
            if (next_boundary > pos && *(next_boundary - 1) == '\n') {
                body_end = next_boundary - 1;
                if (body_end > body_start && *(body_end - 1) == '\r') body_end--;
                break;
            }
            search = next_boundary + delim_len;
        }
        if (!body_end) body_end = data + data_len;

        if (body_end > body_start) {
            part.data = (uint8_t*)(body_start);
            part.data_len = body_end - body_start;
        }

        part.ctx = c;
        handler(&part);

        pos = body_end;
        if (pos < data + data_len - 2 && strncmp(pos, "\r\n", 2) == 0) pos += 2;
        if (pos + end_delim_len <= data + data_len &&
            strncmp(pos + delim_len, "--", 2) == 0) break;
    }
}
