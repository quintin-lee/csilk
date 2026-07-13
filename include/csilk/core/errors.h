#pragma once
/**
 * @file errors.h
 * @brief Standard HTTP status codes and validation flags for csilk.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

/** @name HTTP Status Codes
 *  Standardized constants for common HTTP response status codes.
 *  Use these instead of raw integer literals for readability.
 *  @{ */
enum { CSILK_STATUS_CONTINUE = 100 };
enum { CSILK_STATUS_SWITCHING_PROTOCOLS = 101 };
enum { CSILK_STATUS_OK = 200 };
enum { CSILK_STATUS_CREATED = 201 };
enum { CSILK_STATUS_NO_CONTENT = 204 };
enum { CSILK_STATUS_PARTIAL_CONTENT = 206 };
enum { CSILK_STATUS_MOVED_PERMANENTLY = 301 };
enum { CSILK_STATUS_FOUND = 302 };
enum { CSILK_STATUS_NOT_MODIFIED = 304 };
enum { CSILK_STATUS_TEMPORARY_REDIRECT = 307 };
enum { CSILK_STATUS_BAD_REQUEST = 400 };
enum { CSILK_STATUS_UNAUTHORIZED = 401 };
enum { CSILK_STATUS_PAYMENT_REQUIRED = 402 };
enum { CSILK_STATUS_FORBIDDEN = 403 };
enum { CSILK_STATUS_NOT_FOUND = 404 };
enum { CSILK_STATUS_METHOD_NOT_ALLOWED = 405 };
enum { CSILK_STATUS_REQUEST_TIMEOUT = 408 };
enum { CSILK_STATUS_CONFLICT = 409 };
enum { CSILK_STATUS_GONE = 410 };
enum { CSILK_STATUS_PAYLOAD_TOO_LARGE = 413 };
enum { CSILK_STATUS_RANGE_NOT_SATISFIABLE = 416 };
enum { CSILK_STATUS_URI_TOO_LONG = 414 };
enum { CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE = 415 };
enum { CSILK_STATUS_TOO_MANY_REQUESTS = 429 };
enum { CSILK_STATUS_INTERNAL_SERVER_ERROR = 500 };
enum { CSILK_STATUS_NOT_IMPLEMENTED = 501 };
enum { CSILK_STATUS_BAD_GATEWAY = 502 };
enum { CSILK_STATUS_SERVICE_UNAVAILABLE = 503 };
enum { CSILK_STATUS_GATEWAY_TIMEOUT = 504 };
/** @} */

/** @name Validation flags
 *  Bit flags for use in csilk_valid_rule_t.flags.  Combine with |.
 *  @{ */
enum { CSILK_VALID_REQUIRED = 1 << 0 };
enum { CSILK_VALID_INT = 1 << 1 };
enum { CSILK_VALID_STRING = 1 << 2 };
enum { CSILK_VALID_EMAIL = 1 << 3 };
/** @} */
