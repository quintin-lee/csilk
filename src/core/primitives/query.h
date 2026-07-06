/**
 * @file query.h
 * @brief Internal query/form-urlencoded parsing helpers for csilk.
 *
 * Provides the parsing logic for URL query strings and
 * application/x-www-form-urlencoded request bodies.  These functions
 * are declared CSILK_INTERNAL and called from context.c, h2.c, and http1.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_QUERY_H
#define CSILK_QUERY_H

#include "csilk/csilk.h"

/**
 * @brief Parse a raw query string into the context's query_params hash map.
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
 *       This is called automatically during request finalization.
 */
CSILK_INTERNAL void csilk_parse_query(csilk_ctx_t* c, const char* query_string);

/**
 * @brief Parse the request body as application/x-www-form-urlencoded.
 *
 * Checks the Content-Type header for "application/x-www-form-urlencoded"
 * and, if matched, parses the request body into the form_params hash map.
 * Key-value parsing and URL-decoding follow the same logic as
 * csilk_parse_query().
 *
 * @param c The request context.
 * @note This function must be called explicitly by the handler; it is NOT
 *       invoked automatically. Typically called at the start of a handler
 *       that expects form data.
 */
CSILK_INTERNAL void csilk_parse_form_urlencoded(csilk_ctx_t* c);

/**
 * @brief Get a form urlencoded field value by key.
 *
 * Looks up the given key in the request's form_params hash map, populated
 * by a prior call to csilk_parse_form_urlencoded().
 *
 * @param c   The request context.
 * @param key Field name to look up.
 * @return The URL-decoded field value, or nullptr if not found.
 * @note The returned pointer lives in arena memory.
 */
CSILK_INTERNAL const char* csilk_get_form_field(csilk_ctx_t* c, const char* key);

#endif /* CSILK_QUERY_H */
