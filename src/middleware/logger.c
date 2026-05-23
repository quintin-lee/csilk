/**
 * @file logger.c
 * @brief Request logging middleware — text and JSON structured formats.
 *
 * Uses the csilk reflection engine for JSON serialization of request log entries.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csilk.h"
#include "csilk_reflect.h"

/* ---- reflectable request-log struct ---- */

typedef struct csilk_req_log_s {
    char     method[8];      /**< HTTP method */
    char     path[256];      /**< Request path */
    int32_t  status;         /**< Response status code */
    double   duration;       /**< Request duration in seconds */
    char     remote_addr[46]; /**< Client IP address */
} csilk_req_log_t;

#define REQ_LOG_MAP(X) \
    X(csilk_req_log_t, method,      CSILK_TYPE_STRING, 8,               0, false, NULL) \
    X(csilk_req_log_t, path,        CSILK_TYPE_STRING, 256,             0, false, NULL) \
    X(csilk_req_log_t, status,      CSILK_TYPE_INT32,  sizeof(int32_t), 0, false, NULL) \
    X(csilk_req_log_t, duration,    CSILK_TYPE_DOUBLE, sizeof(double),  0, false, NULL) \
    X(csilk_req_log_t, remote_addr, CSILK_TYPE_STRING, 46,              0, false, NULL)

CSILK_REGISTER_REFLECT(csilk_req_log_t, REQ_LOG_MAP)

/* ---- middleware handler ---- */

void csilk_logger_handler(csilk_ctx_t* c) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    csilk_next(c);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double duration =
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    const char* method = c->request.method ? c->request.method : "UNKNOWN";
    const char* path   = c->request.path   ? c->request.path   : "UNKNOWN";

    if (csilk_log_is_json()) {
        csilk_req_log_t rl;
        memset(&rl, 0, sizeof(rl));
        snprintf(rl.method, sizeof(rl.method), "%s", method);
        snprintf(rl.path,   sizeof(rl.path),   "%s", path);
        rl.status   = (int32_t)c->response.status;
        rl.duration = duration;
        const char* ip = csilk_get_client_ip(c);
        if (ip) snprintf(rl.remote_addr, sizeof(rl.remote_addr), "%s", ip);

        char* json_str = csilk_json_marshal("csilk_req_log_t", &rl);
        cJSON* extra = json_str ? cJSON_Parse(json_str) : NULL;
        free(json_str);
        _csilk_log_structured(CSILK_LOG_INFO, __FILE__, __LINE__, __func__,
                              extra, "request completed");
    } else {
        CSILK_LOG_I("[HTTP] %s %s %d %.6f s",
            method, path, c->response.status, duration);
    }
}
