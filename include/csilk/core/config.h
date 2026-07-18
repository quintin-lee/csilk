#pragma once
/**
 * @file config.h
 * @brief Server and application configuration for the csilk framework.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#include "csilk/core/types.h"

/**
 * @brief Low-level server configuration options.
 *
 * Controls timeouts, resource limits, TCP tuning, and TLS settings.
 * A zero-initialised struct provides safe defaults for most fields.
 * Apply via csilk_server_set_config before calling csilk_server_run.
 */
typedef struct csilk_server_config_s {
    unsigned int idle_timeout_ms;    /**< HTTP keep-alive idle timeout in milliseconds.
                          Connection closed when no new request arrives within
                          this window. 0 = use default (typically 30 s). */
    unsigned int read_timeout_ms;    /**< Maximum time in milliseconds to wait for the full
                          request headers+body (0 = disabled). */
    unsigned int write_timeout_ms;   /**< Maximum time in milliseconds to send the
                                    response (0 = disabled). */
    unsigned int request_timeout_ms; /**< Maximum time in milliseconds for a complete
                               request/response cycle (0 = disabled). Overrides
                               read/write timeouts if set. */
    size_t       max_body_size;      /**< Maximum allowed request body size in bytes.
                               Requests exceeding this get 413 Payload Too Large. */
    size_t       max_header_size;    /**< Maximum total size of all request headers in
                               bytes. */
    size_t       max_url_size;       /**< Maximum URL length in bytes (0 = disabled). */
    size_t       max_headers_count;  /**< Maximum number of individual header fields (0 =
                               unlimited). */
    int          max_connections;    /**< Maximum concurrent client connections (0 =
                               unlimited). */
    int          listen_backlog;     /**< TCP listen(2) backlog hint passed to the kernel. */
    int          tcp_nodelay;        /**< Non-zero enables TCP_NODELAY (disable Nagle's
                         algorithm). */
    int          tcp_keepalive;      /**< TCP keep-alive probe interval in seconds (0 =
                         disabled). */
    int          worker_threads;     /**< Number of worker threads for SO_REUSEPORT listener
                         sockets. 0 = number of CPUs. */

    /* TLS configuration */
    int   enable_tls;      /**< Non-zero enables HTTPS via TLS.  Requires @p
                          tls_cert_file and @p tls_key_file. */
    char* tls_cert_file;   /**< Path to the SSL/TLS certificate file (PEM format).
                          Must be set if @p enable_tls is 1. */
    char* tls_key_file;    /**< Path to the SSL/TLS private key file (PEM format).
                          Must be set if @p enable_tls is 1. */
    char* tls_ca_file;     /**< Path to the CA certificate bundle for
                          client-certificate verification (optional). */
    int   tls_verify_peer; /**< Non-zero to require and verify a client certificate.
                           Requires @p tls_ca_file. */

    /* HTTP/2 server push */
    int h2_push_enable;          /**< Non-zero to enable HTTP/2 server push. */
    int h2_max_push_per_request; /**< Maximum push promises per request (0 = default
                      10). */

    /* Performance & Backpressure optimizations */
    int enable_simd;            /**< Non-zero to enable SIMD-accelerated routing (if supported). */
    int enable_arena_alignment; /**< Non-zero to enable 64-byte cache-line alignment in Arena. */
    size_t
        backpressure_max_queue_depth; /**< Max queued async tasks limit before 503 backpressure (0 = disabled). */
    unsigned int
        backpressure_max_latency_us; /**< Max task latency limit in us before backpressure (0 = disabled). */

    /* Ecosystem */
    int enable_openapi; /**< Non-zero to automatically serve /openapi.json. */
} csilk_server_config_t;

/**
 * @brief Top-level application configuration.
 *
 * Unifies server, logger, middleware, and feature-flag settings.
 * Typically populated from a YAML file via csilk_load_config.
 */
typedef struct {
    int                   port;       /**< TCP port the server listens on. */
    int                   http3_port; /**< UDP port for HTTP/3 (0 = disabled). */
    csilk_server_config_t server;     /**< Low-level server/connection settings. */
    csilk_log_config_t    logger;     /**< Logger initialisation settings. */
    struct {
        int                 enable;   /**< Non-zero to install the CORS middleware. */
        csilk_cors_config_t config;   /**< CORS header values when enabled. */
    } cors;                           /**< Cross-Origin Resource Sharing settings. */
    struct {
        int enable;                   /**< Non-zero to install the rate-limiter middleware. */
        int requests_per_minute;      /**< Maximum requests/minute/IP when enabled. */
    } rate_limit;                     /**< Per-IP rate limiting settings. */
    struct {
        int   enable;                 /**< Non-zero to enable static file serving. */
        char* root_dir;               /**< Absolute or relative path to the local directory to
                       serve. */
        char* prefix;                 /**< URL prefix for static files (e.g., "/static"). */
    } static_files;                   /**< Static file server settings. */
    struct {
        int   enable_logger;          /**< Non-zero to install the request-logging middleware.
                        */
        int   enable_recovery;        /**< Non-zero to install the panic-recovery middleware.
                          */
        int   enable_csrf;            /**< Non-zero to install the CSRF-protection middleware. */
        int   enable_auth;            /**< Non-zero to install the token-auth middleware. */
        char* auth_token;             /**< Expected bearer token when @p enable_auth is 1 (nullptr
                         = disabled even if enabled). */
    } middleware;                     /**< Built-in middleware toggles. */
    struct {
        char* driver;                 /**< AI driver name (e.g., "openai", "claude"). */
        char* model;                  /**< AI model identifier (e.g., "gpt-4", "claude-3"). */
        char* api_key;                /**< API key for the AI service. */
        char* base_url;               /**< Optional base URL for API requests. */
    } ai;                             /**< AI integration settings. */
    struct {
        int   enable;                 /**< Non-zero to enable cipher functionality. */
        char* driver;                 /**< Cipher driver name (e.g., "openssl"). */
    } cipher;                         /**< Cipher/cryptography settings. */
} csilk_config_t;

/**
 * @brief Load and parse a YAML configuration file.
 *
 * Reads a YAML file at @p yaml_path and populates @p config.  All string
 * fields in @p config are heap-allocated and must be freed with
 * csilk_config_free.
 *
 * @param  yaml_path  Path to a YAML configuration file.
 * @param[out] config Pointer to a caller-allocated csilk_config_t to populate.
 * @return 0 on success, -1 on failure (parse error or file not found).
 */
int csilk_load_config(const char* yaml_path, csilk_config_t* config);

/**
 * @brief Validate configuration values for semantic correctness.
 *
 * Checks for out-of-range ports, conflicting settings, missing required
 * paths, etc.
 *
 * @param  config     Pointer to the configuration to validate.
 * @param[out] error_msg  Optional pointer to receive a static error string
 *                        (do NOT free).  Unchanged on success.
 * @return 0 if the configuration is valid, -1 if invalid (@p error_msg is set).
 */
int csilk_config_validate(const csilk_config_t* config, const char** error_msg);

/**
 * @brief Free all heap-allocated strings inside a configuration.
 *
 * Does NOT free the csilk_config_t struct itself (only its members).
 * Safe to call on a zero-initialised struct.
 *
 * @param config Pointer to the configuration struct whose fields should be
 * freed.
 */
void csilk_config_free(csilk_config_t* config);
