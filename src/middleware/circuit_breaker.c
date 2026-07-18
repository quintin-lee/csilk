/**
 * @file circuit_breaker.c
 * @brief Circuit Breaker middleware implementation for downstream protection.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct csilk_circuit_breaker_s {
    int           state; // 0 = CLOSED, 1 = OPEN, 2 = HALF_OPEN
    int           consecutive_failures;
    int           failure_threshold;
    int           recovery_timeout_ms;
    uint64_t      last_state_change_us;
    csilk_mutex_t mutex;
};

csilk_circuit_breaker_t*
csilk_circuit_breaker_new(const csilk_circuit_breaker_config_t* config)
{
    csilk_circuit_breaker_t* cb = malloc(sizeof(csilk_circuit_breaker_t));
    if (!cb) {
        return nullptr;
    }
    cb->state = 0; // CLOSED
    cb->consecutive_failures = 0;
    cb->failure_threshold =
        (config && config->failure_threshold > 0) ? config->failure_threshold : 5;
    cb->recovery_timeout_ms =
        (config && config->recovery_timeout_ms > 0) ? config->recovery_timeout_ms : 5000;
    cb->last_state_change_us = csilk_io_hrtime() / 1000;
    csilk_mutex_init(&cb->mutex);
    return cb;
}

void
csilk_circuit_breaker_middleware(csilk_ctx_t* c, csilk_circuit_breaker_t* cb)
{
    if (!c || !cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    uint64_t now_us = csilk_io_hrtime() / 1000;

    if (cb->state == 1) {  // OPEN
        if ((now_us - cb->last_state_change_us) / 1000 >= (uint64_t)cb->recovery_timeout_ms) {
            cb->state = 2; // HALF_OPEN probe
            cb->last_state_change_us = now_us;
        } else {
            csilk_mutex_unlock(&cb->mutex);
            csilk_json_string(
                c, 503, "{\"error\": \"Service Unavailable: Circuit Breaker is OPEN\"}");
            csilk_abort(c);
            return;
        }
    }
    csilk_mutex_unlock(&cb->mutex);

    csilk_next(c);
}

void
csilk_circuit_breaker_record_success(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    cb->consecutive_failures = 0;
    if (cb->state == 2) { // HALF_OPEN -> CLOSED
        cb->state = 0;
        cb->last_state_change_us = csilk_io_hrtime() / 1000;
    }
    csilk_mutex_unlock(&cb->mutex);
}

void
csilk_circuit_breaker_record_failure(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    cb->consecutive_failures++;
    if (cb->consecutive_failures >= cb->failure_threshold) {
        cb->state = 1; // OPEN
        cb->last_state_change_us = csilk_io_hrtime() / 1000;
    }
    csilk_mutex_unlock(&cb->mutex);
}

int
csilk_circuit_breaker_get_state(const csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return 0;
    }
    return cb->state;
}

void
csilk_circuit_breaker_free(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_destroy(&cb->mutex);
    free(cb);
}
