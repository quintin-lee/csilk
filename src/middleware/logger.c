/**
 * @file logger.c
 * @brief Request logging middleware — text and JSON structured formats.
 * @copyright MIT License
 */

#include <stdio.h>
#include <time.h>
#include "csilk.h"

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
        cJSON* fields = csilk_log_make_kv(
            "method",   method,
            "path",     path,
            "status",   NULL,  /* placeholder for int */
            "duration", NULL,  /* placeholder for float */
            NULL
        );
        if (fields) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", c->response.status);
            cJSON_ReplaceItemInObject(fields, "status",
                cJSON_CreateNumber((double)c->response.status));
            snprintf(buf, sizeof(buf), "%.6f", duration);
            cJSON_ReplaceItemInObject(fields, "duration",
                cJSON_CreateString(buf));

            _csilk_log_structured(CSILK_LOG_INFO,
                __FILE__, __LINE__, __func__,
                fields, "request completed");
        }
    } else {
        CSILK_LOG_I("[HTTP] %s %s %d %.6f s",
            method, path, c->response.status, duration);
    }
}
