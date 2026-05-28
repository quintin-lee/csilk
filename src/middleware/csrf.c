/**
 * @file csrf.c
 * @brief Stateless CSRF protection middleware implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief Stateless CSRF protection middleware (cookie + header token
 *        comparison).
 *
 * On safe HTTP methods (GET, HEAD, OPTIONS), the middleware ensures a CSRF
 * cookie called "csrf_token" is present (generating one if missing) and
 * proceeds to the next handler.
 *
 * On state-changing methods (POST, PUT, DELETE, etc.), it validates the
 * X-CSRF-Token request header against the csrf_token cookie. If the tokens
 * do not match or the header is absent, a 403 Forbidden response is returned
 * and the pipeline is aborted.
 *
 * @param c  The request context.
 *
 * @note Must be registered before any handler that mutates server state.
 * @warning The token is generated from /dev/urandom when available, with a
 *          weak fallback (time + pid). In high-security deployments, ensure
 *          /dev/urandom is accessible.
 */
void
csilk_csrf_middleware(csilk_ctx_t* c)
{
	/* Safe methods (GET, HEAD, OPTIONS) are considered read-only per HTTP spec
     (RFC 7231 §4.2.1). They set the CSRF cookie so the frontend can obtain
     the token, but do not require validation. */
	if (csilk_get_method(c) &&
	    (strcmp(csilk_get_method(c), "GET") == 0 || strcmp(csilk_get_method(c), "HEAD") == 0 ||
	     strcmp(csilk_get_method(c), "OPTIONS") == 0)) {
		// Set CSRF cookie on safe methods so frontend can read it
		const char* existing = csilk_get_cookie(c, "csrf_token");
		if (!existing) {
			char token_buf[33];
			if (csilk_csrf_generate_token(token_buf, sizeof(token_buf)) == 0) {
				csilk_set_cookie(
				    c, "csrf_token", token_buf, 86400, "/", NULL, 0, 1);
			}
		}
		csilk_next(c);
		return;
	}

	const char* token = csilk_get_header(c, "X-CSRF-Token");
	if (!token) {
		void _csilk_metrics_inc_csrf_violations(void);
		_csilk_metrics_inc_csrf_violations();
		csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Forbidden: CSRF token missing");
		csilk_abort(c);
		return;
	}

	/* Double-submit cookie pattern: the server compares a header value against
     the cookie value. This is stateless (no server-side token storage needed)
     but relies on the browser's same-origin policy to prevent the attacker
     from reading/writing the cookie on the target origin. */
	const char* cookie_token = csilk_get_cookie(c, "csrf_token");
	if (cookie_token && strcmp(cookie_token, token) == 0) {
		csilk_next(c);
	} else {
		void _csilk_metrics_inc_csrf_violations(void);
		_csilk_metrics_inc_csrf_violations();
		csilk_json_error(c, CSILK_STATUS_FORBIDDEN, "Forbidden: Invalid CSRF token");
		csilk_abort(c);
	}
}

/**
 * @brief Generate a cryptographically random CSRF token.
 *
 * Reads 16 bytes from /dev/urandom and formats them as a 32-character hex
 * string (plus null terminator). If /dev/urandom cannot be opened, falls
 * back to a weak PRNG seeded with time XOR pid.
 *
 * @param buf      Output buffer to receive the null-terminated hex token.
 * @param buf_size Size of the output buffer. Must be at least 33 bytes.
 *
 * @return 0 on success, -1 if buf is NULL, buf_size < 33, or fread fails.
 *
 * @warning The fallback path uses rand_r() which is NOT cryptographically
 *          secure. Production deployments should always ensure /dev/urandom
 *          is available.
 */
int
csilk_csrf_generate_token(char* buf, size_t buf_size)
{
	if (!buf || buf_size < 33) {
		return -1;
	}

	/* use /dev/urandom for cryptographically random bytes */
	FILE* fp = fopen("/dev/urandom", "rb");
	if (!fp) {
		/* fallback: use time+pid as weak entropy (better than nothing) */
		unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
		snprintf(buf,
			 buf_size,
			 "%08x%08x%08x%08x",
			 rand_r(&seed),
			 rand_r(&seed),
			 rand_r(&seed),
			 rand_r(&seed));
	} else {
		uint8_t random[16];
		if (fread(random, 1, sizeof(random), fp) != sizeof(random)) {
			fclose(fp);
			return -1;
		}
		fclose(fp);
		snprintf(buf,
			 buf_size,
			 "%02x%02x%02x%02x%02x%02x%02x%02x"
			 "%02x%02x%02x%02x%02x%02x%02x%02x",
			 random[0],
			 random[1],
			 random[2],
			 random[3],
			 random[4],
			 random[5],
			 random[6],
			 random[7],
			 random[8],
			 random[9],
			 random[10],
			 random[11],
			 random[12],
			 random[13],
			 random[14],
			 random[15]);
	}
	return 0;
}
