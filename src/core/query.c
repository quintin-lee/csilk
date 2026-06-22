/**
 * @file query.c
 * @brief Query string and form-urlencoded parsing implementation.
 *
 * Parses URL query strings and application/x-www-form-urlencoded request
 * bodies into the context's hash maps.  Both parsers share the same
 * key-value splitting and URL-decoding logic.
 *
 * @copyright MIT License
 */

#include <string.h>

#include "core/ctx_internal.h"
#include "core/header_map.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Parse a raw query string into the context's query_params hash map.
 *
 * Splits the query string on '&' and each key-value pair on '=', URL-decodes
 * both keys and values, and adds them to the request's query_params map.
 * Parameters without a '=' get an empty-string value.
 *
 * @param c            The request context.
 * @param query_string The raw query string (e.g., "foo=1&bar=baz"). The
 *                     leading '?' should NOT be included.
 * @note The query string is duplicated into arena memory before parsing,
 *       so the original string can be freed immediately after this call.
 *       This is called automatically during request finalization. */
void
csilk_parse_query(csilk_ctx_t* c, const char* query_string)
{
	if (!query_string || *query_string == '\0' || !c->arena) {
		return;
	}

	char* qs = csilk_arena_strdup(c->arena, query_string);
	if (!qs) {
		CSILK_LOG_E("Context: failed to allocate arena memory to parse query string: %s",
			    query_string);
		return;
	}

	char* pos = qs;
	while (pos && *pos) {
		char* amp = strchr(pos, '&');
		if (amp) {
			*amp = '\0';
		}

		char* eq = strchr(pos, '=');
		char* key = pos;
		char* value = nullptr;

		if (eq) {
			*eq = '\0';
			value = eq + 1;
		} else {
			value = "";
		}

		if (*key != '\0') {
			csilk_url_decode(key);
			if (value && *value != '\0') {
				csilk_url_decode(value);
			}
			map_add(c, &c->request.query_params, key, value);
			CSILK_LOG_T("Context: parsed query parameter: %s = %s", key, value);
		}

		if (amp) {
			pos = amp + 1;
		} else {
			pos = nullptr;
		}
	}
}

/** @brief Parse the request body as application/x-www-form-urlencoded.
 *
 * Checks the Content-Type header for "application/x-www-form-urlencoded"
 * and, if matched, parses the request body into the form_params hash map.
 * Key-value parsing and URL-decoding follow the same logic as
 * csilk_parse_query().
 *
 * @param c The request context.
 * @note This function must be called explicitly by the handler; it is NOT
 *       invoked automatically. Typically called at the start of a handler
 *       that expects form data. */
void
csilk_parse_form_urlencoded(csilk_ctx_t* c)
{
	if (!c || !c->arena) {
		return;
	}
	const char* body = csilk_get_body(c, nullptr);
	if (!body || *body == '\0') {
		CSILK_LOG_D("Context: csilk_parse_form_urlencoded: empty request body");
		return;
	}

	const char* ct = csilk_get_header(c, "Content-Type");
	if (!ct) {
		CSILK_LOG_D("Context: csilk_parse_form_urlencoded: missing Content-Type");
		return;
	}
	if (strncmp(ct, "application/x-www-form-urlencoded", 33) != 0) {
		CSILK_LOG_D("Context: csilk_parse_form_urlencoded: Content-Type is not "
			    "application/x-www-form-urlencoded (got '%s')",
			    ct);
		return;
	}

	char* qs = csilk_arena_strdup(c->arena, body);
	if (!qs) {
		CSILK_LOG_E("Context: failed to allocate arena memory to parse form urlencoded "
			    "request body");
		return;
	}

	char* pos = qs;
	while (pos && *pos) {
		char* amp = strchr(pos, '&');
		if (amp) {
			*amp = '\0';
		}

		char* eq = strchr(pos, '=');
		char* key = pos;
		char* value = nullptr;

		if (eq) {
			*eq = '\0';
			value = eq + 1;
		} else {
			value = "";
		}

		if (*key != '\0') {
			csilk_url_decode(key);
			if (value && *value != '\0') {
				csilk_url_decode(value);
			}
			map_add(c, &c->request.form_params, key, value);
			CSILK_LOG_T("Context: parsed form parameter: %s = %s", key, value);
		}

		if (amp) {
			pos = amp + 1;
		} else {
			pos = nullptr;
		}
	}
}

/** @brief Get a form urlencoded field value by key.
 *
 * Looks up the given key in the request's form_params hash map, populated
 * by a prior call to csilk_parse_form_urlencoded().
 *
 * @param c   The request context.
 * @param key Field name to look up.
 * @return The URL-decoded field value, or nullptr if not found.
 * @note The returned pointer lives in arena memory. */
const char*
csilk_get_form_field(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}
	return map_get(&c->request.form_params, key);
}
