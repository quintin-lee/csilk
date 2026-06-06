/**
 * @file errors.h
 * @brief Standard HTTP status codes and validation flags for csilk.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_ERRORS_H
#define CSILK_ERRORS_H

/** @name HTTP Status Codes
 *  Standardized constants for common HTTP response status codes.
 *  Use these instead of raw integer literals for readability.
 *  @{ */
static constexpr int CSILK_STATUS_CONTINUE = 100;
static constexpr int CSILK_STATUS_SWITCHING_PROTOCOLS = 101;
static constexpr int CSILK_STATUS_OK = 200;
static constexpr int CSILK_STATUS_CREATED = 201;
static constexpr int CSILK_STATUS_NO_CONTENT = 204;
static constexpr int CSILK_STATUS_PARTIAL_CONTENT = 206;
static constexpr int CSILK_STATUS_MOVED_PERMANENTLY = 301;
static constexpr int CSILK_STATUS_FOUND = 302;
static constexpr int CSILK_STATUS_NOT_MODIFIED = 304;
static constexpr int CSILK_STATUS_TEMPORARY_REDIRECT = 307;
static constexpr int CSILK_STATUS_BAD_REQUEST = 400;
static constexpr int CSILK_STATUS_UNAUTHORIZED = 401;
static constexpr int CSILK_STATUS_PAYMENT_REQUIRED = 402;
static constexpr int CSILK_STATUS_FORBIDDEN = 403;
static constexpr int CSILK_STATUS_NOT_FOUND = 404;
static constexpr int CSILK_STATUS_METHOD_NOT_ALLOWED = 405;
static constexpr int CSILK_STATUS_REQUEST_TIMEOUT = 408;
static constexpr int CSILK_STATUS_CONFLICT = 409;
static constexpr int CSILK_STATUS_GONE = 410;
static constexpr int CSILK_STATUS_PAYLOAD_TOO_LARGE = 413;
static constexpr int CSILK_STATUS_RANGE_NOT_SATISFIABLE = 416;
static constexpr int CSILK_STATUS_URI_TOO_LONG = 414;
static constexpr int CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE = 415;
static constexpr int CSILK_STATUS_TOO_MANY_REQUESTS = 429;
static constexpr int CSILK_STATUS_INTERNAL_SERVER_ERROR = 500;
static constexpr int CSILK_STATUS_NOT_IMPLEMENTED = 501;
static constexpr int CSILK_STATUS_BAD_GATEWAY = 502;
static constexpr int CSILK_STATUS_SERVICE_UNAVAILABLE = 503;
static constexpr int CSILK_STATUS_GATEWAY_TIMEOUT = 504;
/** @} */

/** @name Validation flags
 *  Bit flags for use in csilk_valid_rule_t.flags.  Combine with |.
 *  @{ */
static constexpr int CSILK_VALID_REQUIRED = 1 << 0;
static constexpr int CSILK_VALID_INT = 1 << 1;
static constexpr int CSILK_VALID_STRING = 1 << 2;
static constexpr int CSILK_VALID_EMAIL = 1 << 3;
/** @} */

#endif /* CSILK_ERRORS_H */
