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
enum { MAX_IP_ENTRIES = 65536 };
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

/** @brief djb2 hash of an IP string, mapped into the ip_table index range. */
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

/** @brief Find or atomically claim the lockless rate-limit slot for an IP.
 *
 *  Probes the table starting at the IP's hash index (linear open addressing).
 *  Claims an empty slot via compare-exchange, or returns the slot already
 *  holding the matching IP. On table saturation returns the hash-index slot as
 *  a best-effort fallback. Updates last_seen on a match.
 */
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
                atomic_init(&slot->count, 0);
                atomic_init(&slot->last_reset, now);
                atomic_init(&slot->last_seen, now);
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

    /* Table saturated: return NULL. The caller MUST fail open (skip rate
     * limiting) rather than fall back to a shared slot, which would rate
     * limit unrelated IPs against each other. */
    return NULL;
}

/**
 * @brief Enforce the lockless in-memory rate-limit decision for a given IP.
 *
 * Tracks request counts per client IP within a 60-second sliding window. If the
 * number of requests from a given IP exceeds the limit, a 429 Too Many
 * Requests response is sent (with a Retry-After header) and the pipeline is
 * aborted. On table saturation the request FAILS OPEN (unlimited) rather than
 * sharing a hash slot with unrelated IPs, which would block innocent clients.
 *
 * Extracted from csilk_rate_limit_middleware so the local lockless path can be
 * exercised directly by tests with a fabricated client IP (the middleware
 * resolves the IP from the live socket, which is not available in a mock ctx).
 *
 * @param c     The request context.
 * @param ip    Client IP string (must outlive the call).
 * @param limit Maximum number of requests allowed per IP within the 60-second window.
 */
CSILK_INTERNAL void
_csilk_rate_limit_local(csilk_ctx_t* c, const char* ip, int limit)
{
    time_t             now = time(NULL);
    atomic_ip_entry_t* entry = get_or_create_ip_entry(ip, now);

    if (!entry) {
        /* Table saturated: fail open (unlimited) instead of sharing a slot
         * between unrelated IPs, which would block innocent clients. */
        CSILK_LOG_W("RateLimit: [Local] IP table saturated, skipping rate limiting for IP %s", ip);
        csilk_next(c);
        return;
    }

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
        /* Retry-After = seconds remaining in the current window (at least 1).
         * Re-read last_reset so a window flip racing this request yields the
         * freshly-claimed window start. */
        time_t reset_now = atomic_load(&entry->last_reset);
        time_t retry = (time_t)WINDOW_SIZE - (now - reset_now);
        if (retry < 1) {
            retry = 1;
        }
        char retry_after[32];
        snprintf(retry_after, sizeof(retry_after), "%lld", (long long)retry);
        csilk_set_header(c, "Retry-After", retry_after);
        csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
        csilk_abort(c);
    } else {
        csilk_next(c);
    }
}

/**
 * @brief IP-based rate limiting middleware with lockless sliding window.
 *
 * Resolves the client IP from the live socket, uses the distributed storage
 * path when a storage driver is configured, and otherwise delegates to the
 * lockless in-memory path (see _csilk_rate_limit_local).
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
                /* Distributed store only returns the counter (no TTL); retry with
                 * the full window as a conservative (non-overstated) value. */
                char retry_after[32];
                snprintf(retry_after, sizeof(retry_after), "%lld", (long long)WINDOW_SIZE);
                csilk_set_header(c, "Retry-After", retry_after);
                csilk_json_error(c, CSILK_STATUS_TOO_MANY_REQUESTS, "Too Many Requests");
                csilk_abort(c);
            } else {
                csilk_next(c);
            }
            return;
        }
    }

    _csilk_rate_limit_local(c, ip, limit);
}
