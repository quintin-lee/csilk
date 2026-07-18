/**
 * @file sliding_ratelimit.c
 * @brief Sliding Window Rate Limiting middleware implementation.
 *
 * Implements a weighted sliding window log/counter algorithm:
 * - Divides time into current window and previous window.
 * - Computes estimated rate using linear interpolation: `estimated = prev_count * weight + curr_count`.
 * - Completely prevents boundary burst spikes inherent to traditional fixed-window limiters.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Internal Sliding Window Rate Limiter structure.
 */
struct csilk_sliding_limiter_s {
    int           limit_per_window;        /**< Maximum permitted requests per window */
    uint64_t      window_ms;               /**< Window duration in milliseconds */
    uint64_t      current_window_start_ms; /**< Start timestamp of current window */
    int           prev_window_count;       /**< Request count in previous window */
    int           curr_window_count;       /**< Request count in current window */
    csilk_mutex_t mutex;                   /**< Mutex protecting counter state */
};

/**
 * @brief Create a new Sliding Window Rate Limiter instance.
 *
 * @param limit_per_window Maximum requests allowed within one window.
 * @param window_ms Duration of sliding window in milliseconds (e.g., 60000 for 1 minute).
 * @return Allocated Sliding Limiter handle, or NULL on error.
 */
csilk_sliding_limiter_t*
csilk_sliding_limiter_new(int limit_per_window, uint64_t window_ms)
{
    csilk_sliding_limiter_t* lim = malloc(sizeof(csilk_sliding_limiter_t));
    if (!lim) {
        return nullptr;
    }
    lim->limit_per_window = limit_per_window > 0 ? limit_per_window : 60;
    lim->window_ms = window_ms > 0 ? window_ms : 60000;
    lim->current_window_start_ms = csilk_io_hrtime() / 1000000;
    lim->prev_window_count = 0;
    lim->curr_window_count = 0;
    csilk_mutex_init(&lim->mutex);
    return lim;
}

/**
 * @brief Sliding Window Rate Limiter middleware.
 *
 * Calculates estimated request count based on elapsed time into the current window.
 * Rejects requests exceeding limit with HTTP 429 Too Many Requests and sets Retry-After header.
 *
 * @param c Request context.
 * @param limiter Limiter handle.
 */
void
csilk_sliding_rate_limit_middleware(csilk_ctx_t* c, csilk_sliding_limiter_t* limiter)
{
    if (!c || !limiter) {
        return;
    }

    csilk_mutex_lock(&limiter->mutex);
    uint64_t now_ms = csilk_io_hrtime() / 1000000;
    uint64_t elapsed_ms = now_ms - limiter->current_window_start_ms;

    if (elapsed_ms >= limiter->window_ms * 2) {
        /* More than 2 full windows elapsed -> reset both windows completely */
        limiter->prev_window_count = 0;
        limiter->curr_window_count = 0;
        limiter->current_window_start_ms = now_ms;
        elapsed_ms = 0;
    } else if (elapsed_ms >= limiter->window_ms) {
        /* Slide 1 window forward -> prev = curr, curr = 0 */
        limiter->prev_window_count = limiter->curr_window_count;
        limiter->curr_window_count = 0;
        limiter->current_window_start_ms += limiter->window_ms;
        elapsed_ms = now_ms - limiter->current_window_start_ms;
    }

    /* Calculate weighted rate estimation using time remaining in current window */
    double weight = (double)(limiter->window_ms - elapsed_ms) / (double)limiter->window_ms;
    double estimated_count =
        (double)limiter->prev_window_count * weight + (double)limiter->curr_window_count;

    if (estimated_count >= (double)limiter->limit_per_window) {
        csilk_mutex_unlock(&limiter->mutex);
        csilk_set_header(c, "Retry-After", "1");
        csilk_json_string(
            c, 429, "{\"error\": \"Too Many Requests: Sliding Window Rate Limit Exceeded\"}");
        csilk_abort(c);
        return;
    }

    limiter->curr_window_count++;
    csilk_mutex_unlock(&limiter->mutex);

    csilk_next(c);
}

/**
 * @brief Free resources allocated for a Sliding Window Rate Limiter.
 *
 * @param limiter Limiter handle.
 */
void
csilk_sliding_limiter_free(csilk_sliding_limiter_t* limiter)
{
    if (!limiter) {
        return;
    }
    csilk_mutex_destroy(&limiter->mutex);
    free(limiter);
}
