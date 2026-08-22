#pragma once
/**
 * @file errors.h
 * @brief Standard HTTP status codes and validation flags for csilk.
 *
 * @version 0.5.1
 * @copyright MIT License
 */

/** @name HTTP Status Codes
 *  Standardized constants for common HTTP response status codes.
 *  Use these instead of raw integer literals for readability.
 *  @{ */
enum { CSILK_STATUS_CONTINUE = 100 };               /**< HTTP 100 Continue. */
enum { CSILK_STATUS_SWITCHING_PROTOCOLS = 101 };    /**< HTTP 101 Switching Protocols. */
enum { CSILK_STATUS_OK = 200 };                     /**< HTTP 200 OK. */
enum { CSILK_STATUS_CREATED = 201 };                /**< HTTP 201 Created. */
enum { CSILK_STATUS_NO_CONTENT = 204 };             /**< HTTP 204 No Content. */
enum { CSILK_STATUS_PARTIAL_CONTENT = 206 };        /**< HTTP 206 Partial Content. */
enum { CSILK_STATUS_MOVED_PERMANENTLY = 301 };      /**< HTTP 301 Moved Permanently. */
enum { CSILK_STATUS_FOUND = 302 };                  /**< HTTP 302 Found. */
enum { CSILK_STATUS_NOT_MODIFIED = 304 };           /**< HTTP 304 Not Modified. */
enum { CSILK_STATUS_TEMPORARY_REDIRECT = 307 };     /**< HTTP 307 Temporary Redirect. */
enum { CSILK_STATUS_BAD_REQUEST = 400 };            /**< HTTP 400 Bad Request. */
enum { CSILK_STATUS_UNAUTHORIZED = 401 };           /**< HTTP 401 Unauthorized. */
enum { CSILK_STATUS_PAYMENT_REQUIRED = 402 };       /**< HTTP 402 Payment Required. */
enum { CSILK_STATUS_FORBIDDEN = 403 };              /**< HTTP 403 Forbidden. */
enum { CSILK_STATUS_NOT_FOUND = 404 };              /**< HTTP 404 Not Found. */
enum { CSILK_STATUS_METHOD_NOT_ALLOWED = 405 };     /**< HTTP 405 Method Not Allowed. */
enum { CSILK_STATUS_REQUEST_TIMEOUT = 408 };        /**< HTTP 408 Request Timeout. */
enum { CSILK_STATUS_CONFLICT = 409 };               /**< HTTP 409 Conflict. */
enum { CSILK_STATUS_GONE = 410 };                   /**< HTTP 410 Gone. */
enum { CSILK_STATUS_PAYLOAD_TOO_LARGE = 413 };      /**< HTTP 413 Payload Too Large. */
enum { CSILK_STATUS_RANGE_NOT_SATISFIABLE = 416 };  /**< HTTP 416 Range Not Satisfiable. */
enum { CSILK_STATUS_URI_TOO_LONG = 414 };           /**< HTTP 414 URI Too Long. */
enum { CSILK_STATUS_UNSUPPORTED_MEDIA_TYPE = 415 }; /**< HTTP 415 Unsupported Media Type. */
enum { CSILK_STATUS_TOO_MANY_REQUESTS = 429 };      /**< HTTP 429 Too Many Requests. */
enum { CSILK_STATUS_INTERNAL_SERVER_ERROR = 500 };  /**< HTTP 500 Internal Server Error. */
enum { CSILK_STATUS_NOT_IMPLEMENTED = 501 };        /**< HTTP 501 Not Implemented. */
enum { CSILK_STATUS_BAD_GATEWAY = 502 };            /**< HTTP 502 Bad Gateway. */
enum { CSILK_STATUS_SERVICE_UNAVAILABLE = 503 };    /**< HTTP 503 Service Unavailable. */
enum { CSILK_STATUS_GATEWAY_TIMEOUT = 504 };        /**< HTTP 504 Gateway Timeout. */
/** @} */

/** @name Validation flags
 *  Bit flags for use in csilk_valid_rule_t.flags.  Combine with |.
 *  @{ */
enum { CSILK_VALID_REQUIRED = 1 << 0 }; /**< Field must be present. */
enum { CSILK_VALID_INT = 1 << 1 };      /**< Field must parse as an integer. */
enum { CSILK_VALID_STRING = 1 << 2 };   /**< Field must be a string. */
enum { CSILK_VALID_EMAIL = 1 << 3 };    /**< Field must be a valid email. */
/** @} */
