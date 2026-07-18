/**
 * @file ratelimit.c
 * @brief Lockless IP-based rate limiting middleware implementation.
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"
#include "../core/ctx/ctx_internal.h"

/** @brief Maximum number of distinct IP addresses tracked concurrently. */
enum { MAX_IP_ENTRIES = 1024 };
/** @brief Rate limiting sliding window size in seconds. */
enum { WINDOW_SIZE = 60 };

/**
 * @brief Rate-limit tracking entry for a single IP address (Lockless).
 */
typedef struct {
    char             ip[46];     /**< Client IP address string. */
    _Atomic uint32_t count;      /**< Request count in current window. */
    _Atomic time_t   last_reset; /**< Timestamp when the window started. */
    _Atomic time_t   last_seen;  /**< Timestamp of last request. */
    _Atomic int      in_use;     /**< 0: empty, 1: initializing, 2: ready */
} atomic_ip_entry_t;

static atomic_ip_entry_t ip_table[MAX_IP_ENTRIES];

/* Forward declaration for metrics counter — used in the rate-limit middleware */
extern void _csilk_metrics_inc_rate_limit_blocks(void);

static uint32_t
ip_hash(const char* ip)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*ip++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_IP_ENTRIES;
}

static atomic_ip_entry_t*
get_or_create_ip_entry(const char* ip, time_t now)
{
    uint32_t start_idx = ip_hash(ip);

    for (uint32_t i = 0; i < MAX_IP_ENTRIES; i++) {
        uint32_t           idx = (start_idx + i) % MAX_IP_ENTRIES;
        atomic_ip_entry_t* slot = &ip_table[idx];

        int state = atomic_load(&slot->in_use);
        if (state == 0) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&slot->in_use, &expected, 1)) {
                snprintf(slot->ip, sizeof(slot->ip), "%s", ip);
                atomic_store(&slot->count, 0);
                atomic_store(&slot->last_reset, now);
                atomic_store(&slot->last_seen, now);
                atomic_store(&slot->in_use, 2);
                return slot;
            }
            state = atomic_load(&slot->in_use);
        }

        if (state == 2 && strcmp(slot->ip, ip) == 0) {
            atomic_store(&slot->last_seen, now);
            return slot;
        }
    }

    /* Table saturated — fallback to index hash slot */
    return &ip_table[start_idx];
}

/**
 * @brief IP-based rate limiting middleware with lockless sliding window.
 *
 * Tracks request counts per client IP address within a 60-second sliding window.
 * If the number of requests from a given IP exceeds the limit, a 429 Too
 * Many Requests response is sent (with a Retry-After header) and the
 * pipeline is aborted.
 *
 * @param c     The request context.
 * @param limit Maximum number of requests allowed per IP within the 60-second window.
 */
void
csilk_rate_limit_middleware(csilk_ctx_t* c, int limit)
{
    const char* ip = csilk_get_client_ip(c);
    if (!ip) {
        CSILK_LOG_T("RateLimit: Skipping rate limiting: client IP not available");
        csilk_next(c);
        return;
    }

    CSILK_LOG_T("RateLimit: Checking request %p from IP %s (limit: %d)", (void*)c, ip, limit);

    /* Distributed rate limiting using storage driver */
    if (c->storage_driver) {
        char key[128];
        snprintf(key, sizeof(key), "ratelimit:%s", ip);
        long long current_count = csilk_incr(c, key, WINDOW_SIZE);

        if (current_count >= 0) {
            if (current_count > limit) {
                CSILK_LOG_W("RateLimit: [Distributed] Blocked request from IP %s: current "
                            "count %lld exceeds limit %d",
                            ip,
                            current_count,
                            limit);
                _csilk_metrics_inc_rate_limit_blocks();
                csilk_set_header(c, "Retry-After", "60");
                csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
                csilk_abort(c);
            } else {
                csilk_next(c);
            }
            return;
        }
    }

    /* Lockless in-memory rate limiting fallback */
    time_t             now = time(NULL);
    atomic_ip_entry_t* entry = get_or_create_ip_entry(ip, now);

    time_t   reset = atomic_load(&entry->last_reset);
    uint32_t current_count = 0;

    if (now - reset > WINDOW_SIZE) {
        if (atomic_compare_exchange_strong(&entry->last_reset, &reset, now)) {
            atomic_store(&entry->count, 1);
            current_count = 1;
        } else {
            current_count = atomic_fetch_add(&entry->count, 1) + 1;
        }
    } else {
        current_count = atomic_fetch_add(&entry->count, 1) + 1;
    }

    if ((int)current_count > limit) {
        CSILK_LOG_W("RateLimit: [Local] Blocked request from IP %s: current count %u "
                    "exceeds limit %d",
                    ip,
                    current_count,
                    limit);
        _csilk_metrics_inc_rate_limit_blocks();
        csilk_set_header(c, "Retry-After", "60");
        csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
        csilk_abort(c);
    } else {
        csilk_next(c);
    }
}
