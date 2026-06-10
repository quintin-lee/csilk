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
#include "csilk/csilk.h"

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
 *                be nullptr.
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
void
csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler)
{
	if (!c || !csilk_get_body(c, nullptr) || !handler) {
		CSILK_LOG_W("Multipart: parser skipped - context, body, or handler is missing");
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
		CSILK_LOG_W(
		    "Multipart: parser skipped - Content-Type '%s' is not multipart/form-data",
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

	const char* data = csilk_get_body(c, nullptr);
	size_t data_len = csilk_get_body_len(c);

	CSILK_LOG_D("Multipart: parsed boundary delimiter '%s', payload length: %zu bytes",
		    boundary,
		    data_len);

	const char* pos = data;

	/* Expect the first boundary marker at the very start of the body.
     RFC 2046 §5.1.1: the body must begin with "--boundary\r\n". */
	if (strncmp(pos, delimiter, delim_len) != 0) {
		CSILK_LOG_E("Multipart: invalid payload structure - first boundary marker not "
			    "found at start of body");
		return;
	}
	pos += delim_len;
	if (*pos == '\r') {
		pos++;
	}
	if (*pos == '\n') {
		pos++;
	}

	/* Iterate over each part in the multipart body.
     Each part consists of headers (terminated by \r\n\r\n) followed by
     the part body, then the next boundary delimiter. */
	while (pos < data + data_len) {
		csilk_multipart_part_t part;
		memset(&part, 0, sizeof(part));

		CSILK_LOG_T("Multipart: parsing part at byte offset %td", pos - data);

		/* Parse part headers: Content-Disposition (name + optional filename),
       Content-Type, and any additional headers up to the blank line. */
		while (pos < data + data_len && *pos != '\r') {
			if (strncmp(pos, "Content-Disposition:", 20) == 0) {
				pos += 20;
				while (pos < data + data_len && *pos == ' ') {
					pos++;
				}

				const char* name_start = strstr(pos, "name=\"");
				if (name_start) {
					name_start += 6;
					const char* name_end = strchr(name_start, '"');
					if (name_end) {
						size_t nlen = name_end - name_start;
						if (nlen < CSILK_MAX_PART_NAME) {
							memcpy(part.name, name_start, nlen);
							part.name[nlen] = '\0';
						} else {
							CSILK_LOG_W(
							    "Multipart: part name truncated - "
							    "exceeds limit of %d bytes",
							    CSILK_MAX_PART_NAME);
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
						} else {
							CSILK_LOG_W("Multipart: filename truncated "
								    "- exceeds limit of %d bytes",
								    CSILK_MAX_PART_FILENAME);
						}
					}
				}
			} else if (strncmp(pos, "Content-Type:", 13) == 0) {
				pos += 13;
				while (pos < data + data_len && *pos == ' ') {
					pos++;
				}
				const char* ct_end = strstr(pos, "\r\n");
				size_t ct_len = ct_end ? (size_t)(ct_end - pos) : strlen(pos);
				if (ct_len < sizeof(part.content_type) - 1) {
					memcpy(part.content_type, pos, ct_len);
					part.content_type[ct_len] = '\0';
				}
				if (ct_end) {
					pos = ct_end + 2;
				} else {
					pos += ct_len;
				}
				continue;
			}

			const char* line_end = strstr(pos, "\r\n");
			if (line_end) {
				pos = line_end + 2;
			} else {
				break;
			}
		}

		/* Blank line marks the end of headers (RFC 5322 §2.1). */
		if (pos < data + data_len && strncmp(pos, "\r\n", 2) == 0) {
			pos += 2;
		}

		/* Locate the part body: scan forward for the next boundary delimiter.
       The boundary is preceded by "\n" (and optionally "\r") which must be
       excluded from the part data. If no boundary is found, the rest of the
       body is treated as the final part. */
		const char* body_start = pos;

		const char* body_end = nullptr;
		const char* search = pos;
		while (search < data + data_len) {
			const char* next_boundary = strstr(search, delimiter);
			if (!next_boundary) {
				body_end = data + data_len;
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
			body_end = data + data_len;
		}

		if (body_end > body_start) {
			part.data = (uint8_t*)(body_start);
			part.data_len = body_end - body_start;
		}

		CSILK_LOG_D("Multipart: parsed part - name: '%s', filename: '%s', content-type: "
			    "'%s', size: %zu bytes",
			    part.name,
			    part.filename,
			    part.content_type,
			    part.data_len);

		part.ctx = c;

		CSILK_LOG_T("Multipart: invoking user handler for part '%s'", part.name);
		handler(&part);

		pos = body_end;
		if (pos < data + data_len - 2 && strncmp(pos, "\r\n", 2) == 0) {
			pos += 2;
		}
		if (pos + delim_len > data + data_len) {
			CSILK_LOG_D("Multipart: reached end of payload or remaining data too short "
				    "for boundary");
			break;
		}
		if (strncmp(pos, delimiter, delim_len) != 0) {
			CSILK_LOG_W(
			    "Multipart: parsing break - expected boundary delimiter, got '%c%c...'",
			    pos[0],
			    pos[1]);
			break;
		}
		if (pos + end_delim_len <= data + data_len &&
		    strncmp(pos + delim_len, "--", 2) == 0) {
			CSILK_LOG_D("Multipart: reached final boundary marker '--%s--'", boundary);
			break;
		}
		pos += delim_len;
		if (*pos == '\r') {
			pos++;
		}
		if (*pos == '\n') {
			pos++;
		}
	}

	CSILK_LOG_T("Multipart: parser execution finished for request %p", (void*)c);
}
