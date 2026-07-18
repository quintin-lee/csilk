/**
 * @file circuit_breaker.c
 * @brief Circuit Breaker middleware implementation for downstream microservice resilience.
 *
 * Implements a 3-state Circuit Breaker pattern:
 * - CLOSED (0): Normal operation. All requests pass through. Tracks consecutive failures.
 * - OPEN (1): Service degraded. Requests are immediately rejected with HTTP 503.
 * - HALF_OPEN (2): Probe state after recovery timeout. Allows trial request to check health.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Internal Circuit Breaker state structure.
 */
struct csilk_circuit_breaker_s {
    int           state;                /**< Current state: 0 = CLOSED, 1 = OPEN, 2 = HALF_OPEN */
    int           consecutive_failures; /**< Counter for consecutive downstream failures */
    int           failure_threshold;    /**< Failures required to trip to OPEN */
    int           recovery_timeout_ms;  /**< Cooldown duration in ms before trying HALF_OPEN */
    uint64_t      last_state_change_us; /**< Microsecond timestamp of last state transition */
    csilk_mutex_t mutex;                /**< Mutex protecting concurrent access across workers */
};

/**
 * @brief Create a new Circuit Breaker instance with specified configuration.
 *
 * @param config Pointer to configuration options (threshold, timeout), or NULL for defaults.
 * @return Allocated Circuit Breaker handle, or NULL on allocation failure.
 */
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

/**
 * @brief Circuit Breaker middleware handler.
 *
 * Checks if circuit is OPEN. If cooldown period has elapsed, transitions to HALF_OPEN probe state;
 * otherwise aborts request with HTTP 503 Service Unavailable.
 *
 * @param c Request context.
 * @param cb Circuit Breaker instance.
 */
void
csilk_circuit_breaker_middleware(csilk_ctx_t* c, csilk_circuit_breaker_t* cb)
{
    if (!c || !cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    uint64_t now_us = csilk_io_hrtime() / 1000;

    if (cb->state == 1) { // OPEN state
        if ((now_us - cb->last_state_change_us) / 1000 >= (uint64_t)cb->recovery_timeout_ms) {
            /* Cooldown period elapsed -> transition to HALF_OPEN probe state */
            cb->state = 2; // HALF_OPEN
            cb->last_state_change_us = now_us;
        } else {
            /* Fast-fail request with HTTP 503 */
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

/**
 * @brief Record a successful operation. Resets failure count and closes circuit if HALF_OPEN.
 *
 * @param cb Circuit Breaker instance.
 */
void
csilk_circuit_breaker_record_success(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    cb->consecutive_failures = 0;
    if (cb->state == 2) { // Transition HALF_OPEN -> CLOSED
        cb->state = 0;
        cb->last_state_change_us = csilk_io_hrtime() / 1000;
    }
    csilk_mutex_unlock(&cb->mutex);
}

/**
 * @brief Record a failed operation. Increments failure count and trips to OPEN if threshold reached.
 *
 * @param cb Circuit Breaker instance.
 */
void
csilk_circuit_breaker_record_failure(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_lock(&cb->mutex);
    cb->consecutive_failures++;
    if (cb->consecutive_failures >= cb->failure_threshold) {
        cb->state = 1; // Transition to OPEN
        cb->last_state_change_us = csilk_io_hrtime() / 1000;
    }
    csilk_mutex_unlock(&cb->mutex);
}

/**
 * @brief Query current state of Circuit Breaker.
 *
 * @param cb Circuit Breaker instance.
 * @return 0 = CLOSED, 1 = OPEN, 2 = HALF_OPEN.
 */
int
csilk_circuit_breaker_get_state(const csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return 0;
    }
    return cb->state;
}

/**
 * @brief Free resources associated with a Circuit Breaker instance.
 *
 * @param cb Circuit Breaker instance.
 */
void
csilk_circuit_breaker_free(csilk_circuit_breaker_t* cb)
{
    if (!cb) {
        return;
    }
    csilk_mutex_destroy(&cb->mutex);
    free(cb);
}
