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
#include "csilk/core/sync.h"

enum { CSILK_CURL_DEFAULT_MAX_BYTES = 16 * 1024 * 1024 };

static inline void
_csilk_curl_global_init_cb(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static inline void
csilk_curl_global_init_once(void)
{
    static csilk_once_t s_curl_once = CSILK_ONCE_INIT;
    csilk_once(&s_curl_once, _csilk_curl_global_init_cb);
}

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

typedef size_t (*csilk_curl_write_fn)(void*, size_t, size_t, void*);
typedef void (*csilk_curl_line_cb)(const char* line, void* user_data);

typedef struct {
    char*              line_buf;
    size_t             line_size;
    size_t             line_cap;
    size_t             max_line_bytes;
    csilk_curl_line_cb on_line;
    void*              user_data;
} csilk_curl_line_stream_t;

static inline void
csilk_curl_line_stream_free(csilk_curl_line_stream_t* stream)
{
    if (!stream) {
        return;
    }
    free(stream->line_buf);
    stream->line_buf = NULL;
    stream->line_size = 0;
    stream->line_cap = 0;
}

static inline void
_csilk_curl_line_stream_emit(csilk_curl_line_stream_t* stream)
{
    if (!stream || !stream->line_buf) {
        return;
    }
    while (stream->line_size > 0 && stream->line_buf[stream->line_size - 1] == '\r') {
        stream->line_size--;
    }
    stream->line_buf[stream->line_size] = '\0';
    if (stream->on_line && stream->line_size > 0) {
        stream->on_line(stream->line_buf, stream->user_data);
    }
    stream->line_size = 0;
}

static inline size_t
csilk_curl_line_write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    csilk_curl_line_stream_t* stream = (csilk_curl_line_stream_t*)userp;
    if (!stream || !contents) {
        return 0;
    }
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t realsize = size * nmemb;
    size_t limit =
        stream->max_line_bytes ? stream->max_line_bytes : (size_t)CSILK_CURL_DEFAULT_MAX_BYTES;
    const char* data = (const char*)contents;

    for (size_t i = 0; i < realsize; i++) {
        char c = data[i];
        if (c == '\n') {
            if (stream->line_size > 0) {
                _csilk_curl_line_stream_emit(stream);
            }
        } else {
            if (stream->line_size >= limit) {
                return 0;
            }
            if (stream->line_size + 2 > stream->line_cap) {
                size_t ncap = stream->line_cap == 0 ? 256 : stream->line_cap * 2;
                if (ncap > limit + 1) {
                    ncap = limit + 1;
                }
                if (ncap < stream->line_size + 2) {
                    return 0;
                }
                char* nb = realloc(stream->line_buf, ncap);
                if (!nb) {
                    return 0;
                }
                stream->line_buf = nb;
                stream->line_cap = ncap;
            }
            stream->line_buf[stream->line_size++] = c;
        }
    }
    return realsize;
}

static inline int
_csilk_curl_perform_raw(const char*         method,
                        const char*         url,
                        struct curl_slist*  headers,
                        const char*         json_body,
                        long                connect_timeout_ms,
                        long                total_timeout_ms,
                        csilk_curl_write_fn write_fn,
                        void*               write_data,
                        long*               out_http_code,
                        char*               err_buf,
                        size_t              err_cap)
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
    if (write_fn) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fn);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data);
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
    return _csilk_curl_perform_raw(method,
                                   url,
                                   headers,
                                   json_body,
                                   connect_timeout_ms,
                                   total_timeout_ms,
                                   out_buf ? csilk_curl_write_cb : NULL,
                                   out_buf,
                                   out_http_code,
                                   err_buf,
                                   err_cap);
}

static inline int
csilk_curl_post_stream(const char*               url,
                       struct curl_slist*        headers,
                       const char*               json_body,
                       long                      connect_timeout_ms,
                       long                      total_timeout_ms,
                       csilk_curl_line_stream_t* stream,
                       long*                     out_http_code,
                       char*                     err_buf,
                       size_t                    err_cap)
{
    int rc = _csilk_curl_perform_raw("POST",
                                     url,
                                     headers,
                                     json_body,
                                     connect_timeout_ms,
                                     total_timeout_ms,
                                     stream ? csilk_curl_line_write_cb : NULL,
                                     stream,
                                     out_http_code,
                                     err_buf,
                                     err_cap);
    if (rc == 0 && stream && stream->line_size > 0) {
        _csilk_curl_line_stream_emit(stream);
    }
    return rc;
}

#endif /* CSILK_DRIVERS_CURL_COMMON_H */
