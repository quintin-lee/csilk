/**
 * @file sliding_ratelimit.c
 * @brief Sliding Window Rate Limiting middleware implementation.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct csilk_sliding_limiter_s {
    int           limit_per_window;
    uint64_t      window_ms;
    uint64_t      current_window_start_ms;
    int           prev_window_count;
    int           curr_window_count;
    csilk_mutex_t mutex;
};

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
        // More than 2 windows passed, reset both windows
        limiter->prev_window_count = 0;
        limiter->curr_window_count = 0;
        limiter->current_window_start_ms = now_ms;
        elapsed_ms = 0;
    } else if (elapsed_ms >= limiter->window_ms) {
        // Slide 1 window forward
        limiter->prev_window_count = limiter->curr_window_count;
        limiter->curr_window_count = 0;
        limiter->current_window_start_ms += limiter->window_ms;
        elapsed_ms = now_ms - limiter->current_window_start_ms;
    }

    // Calculate weighted rate estimation
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

void
csilk_sliding_limiter_free(csilk_sliding_limiter_t* limiter)
{
    if (!limiter) {
        return;
    }
    csilk_mutex_destroy(&limiter->mutex);
    free(limiter);
}
