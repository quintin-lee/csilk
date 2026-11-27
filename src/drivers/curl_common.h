#ifndef CSILK_DRIVERS_CURL_COMMON_H
#define CSILK_DRIVERS_CURL_COMMON_H

/**
 * @file curl_common.h
 * @brief Shared libcurl response buffer and safe outbound HTTP helpers.
 *
 * Internal header-only utility shared across optional libcurl-backed drivers
 * (AI drivers, Vector drivers, MCP client). Never included by Web Core targets.
 *
 * @copyright MIT License
 */

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/security.h"

enum { CSILK_CURL_DEFAULT_MAX_BYTES = 16 * 1024 * 1024 };

typedef struct {
    char*  data;
    size_t len;
    size_t max_bytes;
} csilk_curl_buf_t;

static inline size_t
csilk_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    csilk_curl_buf_t* buf = (csilk_curl_buf_t*)userp;
    if (!buf || !contents) {
        return 0;
    }
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t realsize = size * nmemb;
    size_t limit = buf->max_bytes ? buf->max_bytes : (size_t)CSILK_CURL_DEFAULT_MAX_BYTES;
    if (realsize > limit || buf->len > limit - realsize) {
        return 0;
    }

    char* ptr = realloc(buf->data, buf->len + realsize + 1);
    if (!ptr) {
        return 0;
    }
    buf->data = ptr;
    memcpy(buf->data + buf->len, contents, realsize);
    buf->len += realsize;
    buf->data[buf->len] = '\0';
    return realsize;
}

static inline bool
csilk_curl_url_allowed(const char* url)
{
    if (!url || !url[0]) {
        return false;
    }
    const char* strict = getenv("CSILK_STRICT_SSRF");
    bool        allow_private = !(strict && strcmp(strict, "1") == 0);
    return csilk_security_url_is_ssrf_safe(url, allow_private);
}

static inline void
csilk_curl_apply_safe_defaults(CURL* curl, long connect_timeout_ms, long total_timeout_ms)
{
    if (!curl) {
        return;
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(
        curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms > 0 ? connect_timeout_ms : 10000L);
    if (total_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, total_timeout_ms);
    }
}

static inline int
csilk_curl_request_json(const char*        method,
                        const char*        url,
                        struct curl_slist* headers,
                        const char*        json_body,
                        long               connect_timeout_ms,
                        long               total_timeout_ms,
                        csilk_curl_buf_t*  out_buf,
                        long*              out_http_code,
                        char*              err_buf,
                        size_t             err_cap)
{
    if (out_http_code) {
        *out_http_code = 0;
    }
    if (!csilk_curl_url_allowed(url)) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "URL rejected by SSRF/protocol policy");
        }
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "Failed to initialize CURL");
        }
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (method && strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else if (!method || strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (out_buf) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, csilk_curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)out_buf);
    }
    csilk_curl_apply_safe_defaults(curl, connect_timeout_ms, total_timeout_ms);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "%s", curl_easy_strerror(rc));
        }
        curl_easy_cleanup(curl);
        return -1;
    }

    if (out_http_code) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);
    }
    curl_easy_cleanup(curl);
    return 0;
}

#endif /* CSILK_DRIVERS_CURL_COMMON_H */
