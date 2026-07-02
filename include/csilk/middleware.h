/**
 * @file middleware.h
 * @brief Built-in middleware and utility handlers for the csilk framework.
 *
 * Provides pluggable middleware components for logging, CORS, CSRF,
 * rate limiting, authentication, JWT validation, gzip compression,
 * static file serving, SSE, metrics, multipart parsing, request
 * validation, session management, panic recovery, and health checks.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_MIDDLEWARE_H
#define CSILK_MIDDLEWARE_H

#include "csilk/types.h"
#include "csilk/context.h"
#include "csilk/response.h"
#include "csilk/crypto.h"

/**
 * @brief Pass control to the next handler in the middleware/handler chain.
 *
 * Implements the "onion" model: handlers before csilk_next run on the way in,
 * handlers after csilk_next run on the way out (after downstream handlers).
 * Call csilk_is_aborted after returning to check whether a downstream handler
 * called csilk_abort.
 *
 * @param c  The request context.
 */
void csilk_next(csilk_ctx_t* c);

/**
 * @brief Immediately abort the handler chain.
 *
 * No further handlers or middleware run (except code after csilk_next in the
 * current handler that checks csilk_is_aborted).  The response accumulated
 * so far is sent to the client.
 *
 * @param c  The request context.
 */
void csilk_abort(csilk_ctx_t* c);

/** @brief Panic-recovery middleware.
 *
 *  Wraps the handler chain and catches panics or critical errors during request
 *  execution (if `c->panicked` is set). If an error is detected, this middleware
 *  sends a 500 response and logs the error instead of cascading failures.
 *
 *  Should be registered as the outermost middleware.
 *
 *  @param c  The request context. */
void csilk_recovery_handler(csilk_ctx_t* c);

/** @brief Trigger a panic (caught by recovery middleware).
 *
 *  Marks the context as panicked and aborted, stops the handler chain execution,
 *  and triggers the recovery cleanup path.
 *
 *  @param c  The request context. */
void csilk_panic(csilk_ctx_t* c);

/** @brief Logging middleware handler.
 * Logs request method, path, and processing time.
 * @param c The request context. */
void csilk_logger_handler(csilk_ctx_t* c);

/** @brief WAF (Web Application Firewall) middleware.
 *
 *  Inspects request path, query parameters, and headers for common attack
 *  patterns (SQL Injection, XSS, Path Traversal). If a violation is detected,
 *  responds with 403 Forbidden and aborts the chain.
 *
 *  @param c  The request context. */
void csilk_waf_middleware(csilk_ctx_t* c);

/** @brief Request ID middleware.
 * Generates a unique ID for each request and sets X-Request-Id header.
 * @param c The request context. */
void csilk_request_id_middleware(csilk_ctx_t* c);

/** @brief Built-in Health Check handler.
 * Returns a simple JSON response {"status": "up"}.
 * @param c The request context. */
void csilk_health_check_handler(csilk_ctx_t* c);

/** @brief Built-in Readiness Check handler.
 * Performs deep health check (MQ, connections) and returns 200 or 503.
 * @param c The request context. */
void csilk_ready_check_handler(csilk_ctx_t* c);

/**
 * @brief CORS middleware — handles preflight and adds CORS headers.
 *
 * Must be called as a route or group middleware.  For preflight OPTIONS
 * requests the middleware sends the appropriate headers and aborts the chain
 * (status 204).  For other requests the CORS headers are added and the chain
 * continues.
 *
 * @param c      The request context.
 * @param config CORS settings.  Must remain valid for the call duration.
 */
void csilk_cors_middleware(csilk_ctx_t* c, const csilk_cors_config_t* config);

/**
 * @brief Simple per-IP rate-limiting middleware.
 *
 * Uses a fixed-window counter per client IP.  If the limit is exceeded a
 * 429 Too Many Requests response is sent and the handler chain is aborted.
 *
 * @param c     The request context.
 * @param limit Maximum number of requests allowed per minute for a single IP.
 */
void csilk_rate_limit_middleware(csilk_ctx_t* c, int limit);

/**
 * @brief Stateless CSRF protection middleware.
 *
 * Checks for a valid CSRF token in the request (via header or form field)
 * on state-changing methods (POST, PUT, DELETE, PATCH).  If the token is
 * missing or invalid the chain is aborted with 403 Forbidden.
 *
 * @param c  The request context.
 */
void csilk_csrf_middleware(csilk_ctx_t* c);

/**
 * @brief Gzip response compression middleware.
 *
 * If the client advertises gzip/deflate support (Accept-Encoding header),
 * this middleware compresses the response body transparently.  Must be
 * registered as a group-level or server-level middleware that wraps the
 * handler chain.
 *
 * @param c  The request context.
 */
void csilk_gzip_middleware(csilk_ctx_t* c);

/**
 * @brief JWT authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, verifies it,
 * and stores the decoded payload in the context under the key "jwt_payload".
 * If the token is missing or invalid, responds with 401 Unauthorized and
 * aborts the chain.
 *
 * @param c      The request context.
 * @param secret Secret key for token verification.
 */
void csilk_jwt_middleware(csilk_ctx_t* c, const char* secret);

/**
 * @brief Simple token-based authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, passes it to
 * @p validator, and aborts the chain with 401 if validation fails.
 *
 * @param c         The request context.
 * @param validator Callback that inspects the token and returns 1 for valid,
 *                  0 for invalid.  Called synchronously on the event-loop
 * thread.
 */
void csilk_auth_middleware(csilk_ctx_t* c, csilk_auth_validator_t validator);

/**
 * @brief Serve static files from a local directory.
 *
 * Maps the current request path to a file under @p root_dir.  If the file
 * exists and is readable its contents are sent with the appropriate
 * Content-Type.  If not found the handler chain continues (so a 404 handler
 * can pick it up).
 *
 * @note The URL prefix must be stripped from c->request.path before calling
 *       this function (the low-level API uses the raw path).  Use
 *       csilk_app_static() for automatic prefix handling.
 *
 * @param c        The request context.
 * @param root_dir Absolute or relative path to the directory to serve.
 */
void csilk_static(csilk_ctx_t* c, const char* root_dir);

/**
 * @brief Prometheus metrics middleware.
 *
 * Tracks request-level metrics: QPS, latency distribution histogram, and
 * HTTP status code counters.  Should be added early in the middleware chain.
 *
 * @param c   The request context.
 * @param arg Optional config string (currently unused, pass nullptr).
 */
void csilk_metrics_middleware(csilk_ctx_t* c, const char* arg);

/**
 * @brief Prometheus /metrics endpoint handler.
 *
 * Exposes collected metrics in the standard Prometheus text exposition format
 * (content-type: text/plain; version=0.0.4).
 *
 * @param c  The request context.
 */
void     csilk_metrics_handler(csilk_ctx_t* c);
uint64_t csilk_metrics_get_total_requests(void);
uint64_t csilk_metrics_get_total_duration(void);

/**
 * @brief Parse a multipart/form-data request body.
 *
 * Iterates over all parts in the request body (using the Content-Type
 * boundary) and calls @p handler for each.  Files and form fields are
 * treated uniformly — check the filename field to distinguish them.
 *
 * @param c       The request context.
 * @param handler Callback invoked once per part.
 */
void csilk_multipart_parse(csilk_ctx_t* c, csilk_multipart_handler_t handler);

/**
 * @brief Validate request parameters against a set of rules.
 *
 * Iterates through the rule array and checks each field for presence,
 * type, and range constraints.  Returns the name of the first field that
 * fails validation.
 *
 * @param c     The request context.
 * @param rules nullptr-terminated array of csilk_valid_rule_t.  The array must
 *              end with an entry whose field field is nullptr.
 * @return nullptr if all rules pass, or a pointer to the failing @p field name
 *         (the returned pointer points into the rule array, not into the
 * context).
 */
const char* csilk_validate(csilk_ctx_t* c, const csilk_valid_rule_t* rules);

/**
 * @brief Initialise the session subsystem (call once at startup).
 *
 * Must be called before any request handling.  Sets up the session ID
 * generator and storage backend.
 */
void csilk_session_init(void);

/**
 * @brief Start or resume a session for the current request.
 *
 * If the client sent a session cookie, the existing session is loaded.
 * Otherwise a new session is created and a Set-Cookie header is added.
 *
 * @param c  The request context.
 */
void csilk_session_start(csilk_ctx_t* c);

/**
 * @brief Store a value in the session.
 *
 * @param c     The request context.
 * @param key   Key name (copied internally).
 * @param value Opaque value pointer (ownership remains with caller).
 */
void csilk_session_set(csilk_ctx_t* c, const char* key, void* value);

/**
 * @brief Retrieve a value from the session.
 *
 * @param c   The request context.
 * @param key Key name.
 * @return The value previously stored with csilk_session_set, or nullptr if
 *         not found.
 */
void* csilk_session_get(csilk_ctx_t* c, const char* key);

/**
 * @brief Destroy the session and clear the session cookie.
 *
 * Removes all stored session data and sets a Set-Cookie header with an
 * expired session ID to instruct the client to delete it.
 *
 * @param c  The request context.
 */
void csilk_session_destroy(csilk_ctx_t* c);

/**
 * @brief Get the session ID for the current request.
 *
 * @param c  The request context.
 * @return The session ID string, or nullptr if no session is active.
 */
const char* csilk_session_get_id(csilk_ctx_t* c);

void csilk_security_get_stats(csilk_security_stats_t* stats);
void csilk_process_get_stats(csilk_process_stats_t* stats);

/**
 * @brief Generate a cryptographically random CSRF token.
 *
 * Produces a hex-encoded 32-byte (256-bit) random token.
 *
 * @param[out]  buf       Output buffer.  Must be at least 33 bytes for the
 *                        64-character hex string plus NUL terminator.
 * @param       buf_size  Size of @p buf in bytes.
 * @return 0 on success, -1 if @p buf_size is too small or the RNG fails.
 */
int csilk_csrf_generate_token(char* buf, size_t buf_size);

/* --- JWT Generation and Verification --- */

/**
 * @brief Generate a signed JWT token (HS256).
 *
 * Creates a three-part JWT (header.payload.signature) using HMAC-SHA256.
 * The @p payload is used as-is for the claims.
 *
 * @param c       Request context (for crypto-driver access).
 * @param payload cJSON object containing JWT claims (e.g., {"sub":"123"}).
 *                Not modified; ownership stays with caller.
 * @param secret  Secret key string for HMAC signing.
 * @return A heap-allocated JWT string (caller must free), or nullptr on failure.
 */
char* csilk_jwt_generate(csilk_ctx_t* c, cJSON* payload, const char* secret);

/**
 * @brief Verify a JWT token and extract its payload.
 *
 * Validates the signature (HMAC-SHA256), checks the "exp" claim if present,
 * and returns the parsed payload.
 *
 * @param c      Request context (for crypto-driver access).
 * @param token  The JWT string to verify.
 * @param secret Secret key for HMAC verification.
 * @return A heap-allocated cJSON object with the payload claims, or nullptr if
 *         the token is invalid or expired.  Caller must free with cJSON_Delete.
 */
cJSON* csilk_jwt_verify(csilk_ctx_t* c, const char* token, const char* secret);

/* --- Extended JWT API (RS256, ES256 support) --- */

char* csilk_jwt_generate_ex(
    csilk_ctx_t* c, cJSON* payload, const char* key, size_t key_len, csilk_jwt_alg_t algorithm);
cJSON* csilk_jwt_verify_ex(
    csilk_ctx_t* c, const char* token, const char* key, size_t key_len, csilk_jwt_alg_t algorithm);
void
csilk_jwt_middleware_ex(csilk_ctx_t* c, const char* key, size_t key_len, csilk_jwt_alg_t algorithm);

/* --- JWT Payload Accessors --- */
char* csilk_ctx_get_jwt_payload_json(csilk_ctx_t* c);
void  csilk_ctx_cleanup_jwt_payload(csilk_ctx_t* c);
char* csilk_jwt_generate_json(csilk_ctx_t* c, const char* payload_json, const char* secret);

#endif /* CSILK_MIDDLEWARE_H */
