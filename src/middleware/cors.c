/**
 * @file cors.c
 * @brief CORS middleware implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief CORS middleware — sets cross-origin headers and handles preflight
 *        requests.
 *
 * Applies the configured Access-Control-* headers to every response. When the
 * Vary: Origin header is set for non-wildcard origins. If the incoming request
 * is an OPTIONS preflight (indicated by Access-Control-Request-Method), the
 * middleware short-circuits with a 204 No Content response instead of
 * forwarding to downstream handlers.
 *
 * @param c       The request context.
 * @param config  Pointer to a CORS configuration struct specifying allowed
 *                origins, methods, headers, credentials, and max-age. Must
 *                remain valid for the duration of the call.
 *
 * @note This middleware always calls csilk_next() for non-preflight requests
 *       after setting response headers.
 * @warning The config pointer is not deep-copied; the caller must ensure it
 *          lives long enough for the current request.
 */
void
csilk_cors_middleware(csilk_ctx_t* c, const csilk_cors_config_t* config)
{
	if (!c || !config) {
		if (c) {
			csilk_next(c);
		}
		return;
	}

	/* Per the Fetch spec, Vary: Origin must be set for non-wildcard origins
     so caches do not serve CORS responses across different origins. */
	csilk_set_header(c, "Access-Control-Allow-Origin", config->allow_origin);
	if (strcmp(config->allow_origin, "*") != 0) {
		csilk_set_header(c, "Vary", "Origin");
	}
	csilk_set_header(c, "Access-Control-Allow-Methods", config->allow_methods);
	csilk_set_header(c, "Access-Control-Allow-Headers", config->allow_headers);

	if (config->allow_credentials) {
		csilk_set_header(c, "Access-Control-Allow-Credentials", "true");
	}

	if (config->max_age > 0) {
		char buf[32];
		int n = snprintf(buf, sizeof(buf), "%d", config->max_age);
		if (n > 0 && (size_t)n < sizeof(buf)) {
			csilk_set_header(c, "Access-Control-Max-Age", buf);
		}
	}

	/* Preflight detection: OPTIONS + Access-Control-Request-Method indicates
     a CORS preflight request (Fetch §4.6). Short-circuit with 204 instead
     of forwarding to the actual route handler. */
	const char* req_method = csilk_get_header(c, "Access-Control-Request-Method");
	if (csilk_get_method(c) && strcmp(csilk_get_method(c), "OPTIONS") == 0 && req_method) {
		csilk_string(c, CSILK_STATUS_NO_CONTENT, "");
		csilk_abort(c);
		return;
	}
	csilk_next(c);
}
