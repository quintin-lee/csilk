/**
 * @file errors.h
 * @brief Standard HTTP status code macros for the csilk framework.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_ERRORS_H
#define CSILK_ERRORS_H

/** @name HTTP Status Codes
 *  Standardized macros for common HTTP response status codes.
 *  Use these instead of raw integer literals for readability.
 *  @{ */
#define CSILK_STATUS_CONTINUE 100
#define CSILK_STATUS_SWITCHING_PROTOCOLS 101
#define CSILK_STATUS_OK 200
#define CSILK_STATUS_CREATED 201
#define CSILK_STATUS_NO_CONTENT 204
#define CSILK_STATUS_PARTIAL_CONTENT 206
#define CSILK_STATUS_MOVED_PERMANENTLY 301
#define CSILK_STATUS_FOUND 302
#define CSILK_STATUS_NOT_MODIFIED 304
#define CSILK_STATUS_TEMPORARY_REDIRECT 307
#define CSILK_STATUS_BAD_REQUEST 400
#define CSILK_STATUS_UNAUTHORIZED 401
#define CSILK_STATUS_PAYMENT_REQUIRED 402
#define CSILK_STATUS_FORBIDDEN 403
#define CSILK_STATUS_NOT_FOUND 404
#define CSILK_STATUS_METHOD_NOT_ALLOWED 405
#define CSILK_STATUS_REQUEST_TIMEOUT 408
#define CSILK_STATUS_CONFLICT 409
#define CSILK_STATUS_GONE 410
#define CSILK_STATUS_PAYLOAD_TOO_LARGE 413
#define CSILK_STATUS_RANGE_NOT_SATISFIABLE 416
#define CSILK_STATUS_URI_TOO_LONG 414
#define CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE 415
#define CSILK_STATUS_TOO_MANY_REQUESTS 429
#define CSILK_STATUS_INTERNAL_SERVER_ERROR 500
#define CSILK_STATUS_NOT_IMPLEMENTED 501
#define CSILK_STATUS_BAD_GATEWAY 502
#define CSILK_STATUS_SERVICE_UNAVAILABLE 503
#define CSILK_STATUS_GATEWAY_TIMEOUT 504
/** @} */

/** @name Validation flags
 *  Bit flags for use in csilk_valid_rule_t.flags.  Combine with |.
 *  @{ */
#define CSILK_VALID_REQUIRED (1 << 0) /**< Field must be present (non-nullptr, non-empty). */
#define CSILK_VALID_INT (1 << 1)      /**< Value must parse as a valid integer. */
#define CSILK_VALID_STRING                                                                         \
	(1 << 2) /**< Value must be a string (always true for form/query values; \
              included for symmetry). */
#define CSILK_VALID_EMAIL                                                                          \
	(1 << 3) /**< Value must match a basic email format (contains '@' and a \
              dot). */
/** @} */

#endif /* CSILK_ERRORS_H */
