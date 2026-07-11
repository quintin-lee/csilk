/**
 * @file logger.c
 * @brief Request logging middleware — text and JSON structured formats.
 *
 * Uses the csilk reflection engine for JSON serialization of request log
 * entries.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"

/* ------------------------------------------------------------------ */
/*  Reflectable request-log struct                                    */
/*                                                                    */
/*  The CSILK_REGISTER_REFLECT macro generates JSON marshalling code  */
/*  at compile time so the struct fields are mapped to JSON keys      */
/*  automatically, avoiding manual serialisation boilerplate.         */
/* ------------------------------------------------------------------ */

/**
 * @brief Request log entry for JSON structured logging.
 *
 * This struct captures the essential fields of an HTTP request for log
 * output. It is registered with the CSILK reflection engine (via
 * CSILK_REGISTER_REFLECT) so it can be automatically marshalled to JSON
 * for structured log entries.
 */
typedef struct csilk_req_log_s {
    char    method[8];       /**< HTTP method */
    char    path[256];       /**< Request path */
    int32_t status;          /**< Response status code */
    double  duration;        /**< Request duration in seconds */
    char    remote_addr[46]; /**< Client IP address */
} csilk_req_log_t;

#define REQ_LOG_MAP(X)                                                                             \
    X(csilk_req_log_t, method, CSILK_TYPE_STRING, 8, 0, false, nullptr)                            \
    X(csilk_req_log_t, path, CSILK_TYPE_STRING, 256, 0, false, nullptr)                            \
    X(csilk_req_log_t, status, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)               \
    X(csilk_req_log_t, duration, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, nullptr)             \
    X(csilk_req_log_t, remote_addr, CSILK_TYPE_STRING, 46, 0, false, nullptr)

CSILK_REGISTER_REFLECT(csilk_req_log_t, REQ_LOG_MAP)

/* ---- middleware handler ---- */

/**
 * @brief Request logging middleware — logs method, path, status, and duration.
 *
 * Records the start time of the request using a high-resolution monotonic
 * clock (CLOCK_MONOTONIC), proceeds to the next handler via csilk_next(),
 * and then calculates the total elapsed time. It automatically selects
 * between plain-text logging and structured JSON logging based on the
 * global logger configuration.
 *
 * In JSON mode, the request details are marshalled into a csilk_req_log_t
 * struct and logged via the reflection-based JSON logger. In text mode, a
 * simple "[HTTP] METHOD PATH STATUS DURATION" line is emitted.
 *
 * If a request ID is set on the context, it is propagated to the logger's
 * thread-local state for correlating log entries.
 *
 * @param c  The request context.
 *
 * @note This middleware should be one of the first in the pipeline so that
 *       the timing covers the entire request lifecycle.
 * @warning csilk_next() is called BEFORE the log output; the response
 *          status must therefore be set by downstream handlers before
 *          returning.
 */
void
csilk_logger_handler(csilk_ctx_t* c)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    csilk_next(c);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double duration = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    const char* method = csilk_get_method(c) ? csilk_get_method(c) : "UNKNOWN";
    const char* path = csilk_get_path(c) ? csilk_get_path(c) : "UNKNOWN";
    const char* rid = csilk_get_request_id(c);

    if (csilk_log_is_json()) {
        if (rid && rid[0] != '\0') {
            csilk_log_set_request_id(rid);
        }
        csilk_req_log_t rl;
        memset(&rl, 0, sizeof(rl));
        snprintf(rl.method, sizeof(rl.method), "%s", method);
        snprintf(rl.path, sizeof(rl.path), "%s", path);
        rl.status = (int32_t)csilk_get_status(c);
        rl.duration = duration;
        const char* ip = csilk_get_client_ip(c);
        if (ip) {
            snprintf(rl.remote_addr, sizeof(rl.remote_addr), "%s", ip);
        }

        char*  json_str = csilk_json_marshal("csilk_req_log_t", &rl);
        cJSON* extra = json_str ? cJSON_Parse(json_str) : nullptr;
        free(json_str);
        _csilk_log_structured(
            CSILK_LOG_INFO, __FILE__, __LINE__, __func__, extra, "request completed");
    } else {
        if (rid && rid[0] != '\0') {
            csilk_log_set_request_id(rid);
        }
        CSILK_LOG_I("[HTTP] %s %s %d %.6f s", method, path, csilk_get_status(c), duration);
    }
}
